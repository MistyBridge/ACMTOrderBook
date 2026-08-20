// clickhouse_source.cpp — ClickHouse 数据源实现 (深交所 + 上交所)
// SPSC 架构: 生产者线程拉「逐笔单流」(服务端 UNION ALL 按序列键排序) 并按时间
// 插入整拉驻留的快照, 推入无锁环形队列; 消费者 (hasNext/next) 顺序取。
// 语义与全量排序版逐位一致。
#include "clickhouse_source.h"
#include "ch_client.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>

#ifdef _MSC_VER
#include <intrin.h>
inline void cpuPause() { _mm_pause(); }
#else
inline void cpuPause() { __asm__ __volatile__("pause" ::: "memory"); }
#endif

namespace source {

// ---- 行解析辅助 ----
static std::vector<std::string> splitTsv(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= line.size()) {
        size_t p = line.find('\t', start);
        if (p == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

// "YYYY-MM-DD HH:MM:SS.mmm" -> YYYYMMDDHHMMSSsss (十进制, 北京时间)
static uint64_t tsToInt(const std::string& s) {
    int y, mo, d, h, mi, se, ms;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d, &h, &mi, &se, &ms) != 7)
        throw std::runtime_error("bad timestamp: " + s);
    return (uint64_t)y * 10000000000000ULL + (uint64_t)mo * 100000000000ULL +
           (uint64_t)d * 1000000000ULL + (uint64_t)h * 10000000ULL +
           (uint64_t)mi * 100000ULL + (uint64_t)se * 1000ULL + (uint64_t)ms;
}

// 由时刻推导交易阶段 Code0 (深沪同表, 由时间推导)
static uint8_t tradingPhaseCode0(uint64_t tt) {
    uint64_t hms = (tt % 1000000000ULL) / 1000;  // HHMMSS
    if (hms < 91500)  return 0;   // 'S' 启动
    if (hms < 92500)  return 1;   // 'O' 开盘集合竞价
    if (hms < 93000)  return 3;   // 'B' 休市
    if (hms < 113000) return 2;   // 'T' 连续
    if (hms < 130000) return 3;   // 'B' 休市
    if (hms < 145700) return 2;   // 'T' 连续
    if (hms < 150000) return 4;   // 'C' 收盘集合竞价
    return 5;                      // 'E' 闭市
}

// CH side (66='B' 83='S' 32=' ') -> AX-SBE 方向字符
static uint8_t sideChar(int64_t side, bool sse) {
    if (side == 66) return sse ? (uint8_t)'B' : (uint8_t)'1';
    if (side == 83) return sse ? (uint8_t)'S' : (uint8_t)'2';
    if (side == 32) return (uint8_t)'N';   // 无方向 (SSE 成交/集合竞价)
    throw std::runtime_error("bad side: " + std::to_string(side));
}

// CH order_type 首字节 ('0'/'1'/'U' 深; 'A'/'D' 沪) -> AX-SBE OrdType
static uint8_t ordTypeChar(int64_t orderType, bool sse) {
    char c = (char)(orderType / 256);
    if (sse) {
        if (c == 'A' || c == 'D') return (uint8_t)c;
        throw std::runtime_error("bad SSE order_type: " + std::to_string(orderType));
    }
    if (c == '0') return (uint8_t)'2';
    if (c == '1') return (uint8_t)'1';
    if (c == 'U') return (uint8_t)'U';
    throw std::runtime_error("bad SZSE order_type: " + std::to_string(orderType));
}

ClickHouseSource::ClickHouseSource(const std::string& host, int port,
                                   const std::string& user, const std::string& password)
    : host_(host), port_(port), user_(user), pass_(password) {}

ClickHouseSource::~ClickHouseSource() {
    stopProducer();
}

// ---- 生产者线程 ----

// 推入 SPSC 通道; 满则 PAUSE 自旋等待 (消费者处理慢于生产, 回测场景无丢弃)。
// 参考 pipeline/producer.cpp: 自旋不离开 CPU, 避免 sched_yield 在高负载机器上
// 让位后长时间排队 (yield 泥潭, 实测 333 事件/秒 → 几十万事件/秒)。
static void pushWait(SpscQueue<Event>& q, const Event& ev, const std::atomic<bool>& stop) {
    for (;;) {
        if (q.push(ev)) return;
        if (stop.load()) return;
        for (int i = 0; i < 64; ++i) cpuPause();
        std::atomic_thread_fence(std::memory_order_acquire);
    }
}

void ClickHouseSource::producerLoop() {
    try {
        for (;;) {
            if (stop_.load()) break;
            // 拉下一条逐笔 (从全量驻留正文取行; 流尽置 incDone_)
            if (!hasInc_ && !incDone_) {
                size_t nl = incBody_.find('\n', incPos_);
                if (nl != std::string::npos) {
                    std::string line = incBody_.substr(incPos_, nl - incPos_);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    incPos_ = nl + 1;
                    incLook_ = parseIncLine(line);
                    hasInc_ = true;
                } else incDone_ = true;
            }
            // 快照插入: 逐笔时间 ≤ 快照时间 → 先发逐笔 (快照排在其后);
            // 逐笔时间 > 快照时间 或 逐笔流尽 → 发快照
            // [史] 曾试 t < st (同毫秒逐笔排后): 集合竞价盘口错位 (600584
            // call 61 vs 基线 254); 曾试消息类型细分 (委托≤/成交<): 更差。
            // 参照实现 (全量归并版 mergeChrono) 语义即 t <= st — 同毫秒逐笔
            // (委托+成交) 全部先发, 快照键窗口 [key,key+2] 吸收越窗帧。
            if (!snapAll_) {
                uint64_t st = snaps_[snapPos_].snap.TransactTime;
                if (hasInc_) {
                    uint64_t t = (incLook_.type == MsgType_order)
                                     ? incLook_.order.TransactTime
                                     : incLook_.exe.TransactTime;
                    if (t <= st) {
                        pushWait(q_, incLook_, stop_);
                        hasInc_ = false;
                        continue;
                    }
                }
                pushWait(q_, snaps_[snapPos_], stop_);
                snapPos_++;
                if (snapPos_ == snaps_.size()) snapAll_ = true;
                continue;
            }
            // 快照发完: 纯逐笔尾部
            if (hasInc_) { pushWait(q_, incLook_, stop_); hasInc_ = false; continue; }
            if (!incDone_) continue;
            break;   // 三路皆尽
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(doneM_);
        prodError_ = e.what();
    }
    std::lock_guard<std::mutex> lk(doneM_);
    prodDone_ = true;
}

void ClickHouseSource::stopProducer() {
    if (!producerTh_.joinable()) return;
    stop_.store(true);
    producerTh_.join();
}

// ---- 消费者侧 (订单簿引擎): 从 SPSC 通道顺序取 ----

bool ClickHouseSource::hasNext() {
    if (hasReady_) return true;
    if (!loaded_) return false;
    for (;;) {
        // 批缓冲还有货: 直接取 (不触碰队列)
        if (batchPos_ < batchCnt_) {
            readyEv_ = batch_[batchPos_++];
            hasReady_ = true;
            return true;
        }
        // 批取: 一次取空队列 (Disruptor 批处理 — 每批只经历一次等待)
        size_t n = q_.pop_batch(batch_, CONSUMER_BATCH);
        if (n > 0) { batchPos_ = 0; batchCnt_ = n; continue; }
        // 队列空: 检查生产者; 自旋等待 (生产者线程在独立核上拉取)
        std::lock_guard<std::mutex> lk(doneM_);
        if (prodDone_) {
            if (!prodError_.empty())
                throw std::runtime_error("ClickHouse 拉取失败: " + prodError_);
            return false;
        }
        for (int i = 0; i < 64; ++i) cpuPause();
        std::atomic_thread_fence(std::memory_order_acquire);
    }
}

int ClickHouseSource::next(AxsbeOrder& o, AxsbeExe& e, AxsbeSnapStock& s) {
    if (!hasReady_ && !hasNext()) return 0;
    int type = readyEv_.type;
    if (type == MsgType_order) { o = readyEv_.order; orderCnt_++; }
    else if (type == MsgType_exe) { e = readyEv_.exe; exeCnt_++; }
    else                          { s = readyEv_.snap; snapCnt_++; }
    hasReady_ = false;
    return type;
}

void ClickHouseSource::load(const std::string& date, const std::string& instrument, int exchange) {
    if (exchange != 1 && exchange != 2)
        throw std::runtime_error("ClickHouseSource: exchange 仅支持 1=上交所 2=深交所");
    // 状态复位 (允许同一对象重复 load)
    stopProducer();
    hasInc_ = false;
    snapPos_ = 0;
    snapAll_ = false;
    hasReady_ = false;
    batchPos_ = batchCnt_ = 0;
    snapCnt_ = orderCnt_ = exeCnt_ = 0;

    sse_ = (exchange == 1);
    // CH 原始值域 -> 交易所原生精度 (解析换算与全量版一致; 引擎内部 ×10^5 归一化在 axob_*)
    // 数据实测 (600584 20260716): 沪市三表价格统一 ×10^4 —
    //   TICK last=875800 = 87.58 元, 涨停 upper_limit=1017100 = 101.71 元;
    //   ORDER/TRANSACTION 同域 (867000 = 86.70 元)。统一 ÷10 进引擎 ×10^3 域。
    pxDiv_     = sse_ ? 10 : 1;    // 沪 ×10^4 ÷10 -> ×10^3; 深 ×10^4 原样
    // 数量域 (数据实测 + 引擎 axsbe_base.h 定点常量对齐):
    //   ORDER/TRANSACTION: 深沪均为百股(×10^2)。深: 引擎输入原样
    //     (qtySnap2Inter ×1000 → 内部 ×10^5); 沪: 解析 ×10 → ×10^3 域,
    //     引擎 ×100 → 内部 ×10^5 (与 genTradingSnap 输出 qtyInter2Snap ÷100 同域)。
    //   TICK: 深沪均为百股(×10^2), 沪 ×10 → ×10^3 域。
    //   [史] 曾设沪 qtyMulInc_=1000 (误以为沪逐笔为股) → 簿量放大 100 倍
    //     (600584 全帧全等失败 0/4740)。核对: 拍卖买单 3,276,500 百股 =
    //     撮合 2,679,814 + 残余 286,086 (TICK b0) + 撤单 310,600, 分毫不差。
    qtyMulInc_  = sse_ ? 10 : 1;       // 逐笔: 沪百股×10(→×10^3 域) / 深百股原样
    qtyMulSnap_ = sse_ ? 10 : 1;       // 快照: 沪百股×10(→×10^3 域) / 深百股原样
    amtMul_ = sse_ ? 10 : 1;       // 额 ×10^4 -> ×10^5(沪) / ×10^4(深)
    securityId_ = std::stoi(instrument.substr(0, 6));

    std::string ins = instrument;
    ins.push_back('\0');
    ins.push_back('\0');

    // 快照整拉驻留 (TICK 表 98KB 级, 秒级)
    std::string sqlSnap =
        "SELECT timestamp, pre_close, open, high, low, last, num_trades, volume, turnover, "
        "total_bid_vol, total_ask_vol, upper_limit, lower_limit, w_avg_bid_p, w_avg_ask_p, "
        "bid_p0, bid_v0, ask_p0, ask_v0, bid_p1, bid_v1, ask_p1, ask_v1, "
        "bid_p2, bid_v2, ask_p2, ask_v2, bid_p3, bid_v3, ask_p3, ask_v3, "
        "bid_p4, bid_v4, ask_p4, ask_v4, bid_p5, bid_v5, ask_p5, ask_v5, "
        "bid_p6, bid_v6, ask_p6, ask_v6, bid_p7, bid_v7, ask_p7, ask_v7, "
        "bid_p8, bid_v8, ask_p8, ask_v8, bid_p9, bid_v9, ask_p9, ask_v9 "
        "FROM LEVEL2.TICK_" + date + " WHERE instrument='" + ins + "' AND exchange=" +
        std::to_string(exchange) + " ORDER BY timestamp";
    {
        ch::QueryReader snapRd(host_, port_, user_, pass_, sqlSnap);
        snaps_.clear();
        std::string line;
        while (snapRd.nextLine(line)) snaps_.push_back(parseSnapLine(line));
        snapAll_ = snaps_.empty();
    }

    // 逐笔单流: ORDER+TRANSACTION 服务端 UNION ALL 并按序列键排序 —
    // 深: seq_no 全局唯一; 沪: biz_index 跨表共享 (数据验证: 与时间戳序等价,
    // 同毫秒内唯一裁决)。客户端只面对一个有序输入, 无两路归并。
    // [v3修复] ORDER BY 必须包在子查询外: ClickHouse 对 "A UNION ALL B ORDER BY x"
    // 的尾随 ORDER BY 是按分支各自排序的 (实测: 输出 = [A 全量][B 全量], 无全局
    // 归并), 导致成交全部排在委托之后、快照时间插入点全错 (撮合 09:25:00.000 的
    // 成交在流末尾才到达, 簿永远不被成交削减)。子查询包裹后才是真正的全局排序。
    std::string sqlIncs =
        sse_
        ? ("SELECT * FROM ("
           "SELECT seq_no, timestamp, price, quantity, side, 1 AS kind, order_type, order_orig_no, 0 AS trade_type, 0 AS bid_no, 0 AS ask_no, biz_index "
           "FROM LEVEL2.ORDER_" + date + " WHERE instrument='" + ins + "' AND exchange=1 "
           "UNION ALL "
           "SELECT seq_no, timestamp, price, quantity, side, 2 AS kind, 0 AS order_type, 0 AS order_orig_no, trade_type, bid_no, ask_no, biz_index "
           "FROM LEVEL2.TRANSACTION_" + date + " WHERE instrument='" + ins + "' AND exchange=1) "
           "ORDER BY biz_index")
        : ("SELECT * FROM ("
           "SELECT seq_no, timestamp, price, quantity, side, 1 AS kind, order_type, 0 AS order_orig_no, 0 AS trade_type, 0 AS bid_no, 0 AS ask_no, 0 AS biz_index "
           "FROM LEVEL2.ORDER_" + date + " WHERE instrument='" + ins + "' AND exchange=2 "
           "UNION ALL "
           "SELECT seq_no, timestamp, price, quantity, side, 2 AS kind, 0 AS order_type, 0 AS order_orig_no, trade_type, bid_no, ask_no, 0 AS biz_index "
           "FROM LEVEL2.TRANSACTION_" + date + " WHERE instrument='" + ins + "' AND exchange=2) "
           "ORDER BY seq_no");

    // [v3.3] 逐笔单流批量全量拉取 (query 物化驻留): 流式 QueryReader 挂 CH
    // 长连接, 队列满背压时 CH send 阻塞超时断连 (600584 177 万行实测截断);
    // 基线 (全量 query) 从未失败。139MB 级内存驻留可接受。
    incBody_ = ch::query(host_, port_, user_, pass_, sqlIncs);   // 全量物化
    incPos_ = 0;
    loaded_ = true;

    {
        std::lock_guard<std::mutex> lk(doneM_);
        prodDone_ = false;
        prodError_.clear();
    }
    stop_.store(false);
    producerTh_ = std::thread(&ClickHouseSource::producerLoop, this);
}

// ---- 行解析 (逻辑与原 fetchTicks/fetchIncs 逐行一致) ----

Event ClickHouseSource::parseSnapLine(const std::string& line) {
    std::vector<std::string> f = splitTsv(line);
    if (f.size() < 55) throw std::runtime_error("TICK 行字段数不足: " + line.substr(0, 64));

    AxsbeSnapStock s{};
    s.secSrc = sse_ ? SecurityIDSource_SSE : SecurityIDSource_SZSE;
    s.securityID = securityId_;
    s.ChannelNo = 2013;
    uint64_t tt = tsToInt(f[0]);
    s.TradingPhaseCode = tradingPhaseCode0(tt);
    s.NumTrades        = atoll(f[6].c_str());
    s.TotalVolumeTrade = atoll(f[7].c_str()) * qtyMulSnap_;
    s.TotalValueTrade  = atoll(f[8].c_str()) * amtMul_;
    s.PrevClosePx      = (int32_t)(atoll(f[1].c_str()) / pxDiv_);
    s.LastPx           = (int32_t)(atoll(f[5].c_str()) / pxDiv_);
    s.OpenPx           = (int32_t)(atoll(f[2].c_str()) / pxDiv_);
    s.HighPx           = (int32_t)(atoll(f[3].c_str()) / pxDiv_);
    s.LowPx            = (int32_t)(atoll(f[4].c_str()) / pxDiv_);
    s.BidWeightPx      = (int32_t)(atoll(f[13].c_str()) / pxDiv_);
    s.BidWeightSize    = atoll(f[9].c_str()) * qtyMulSnap_;
    s.AskWeightPx      = (int32_t)(atoll(f[14].c_str()) / pxDiv_);
    s.AskWeightSize    = atoll(f[10].c_str()) * qtyMulSnap_;
    s.UpLimitPx        = (int32_t)(atoll(f[11].c_str()) / pxDiv_);
    s.DnLimitPx        = (int32_t)(atoll(f[12].c_str()) / pxDiv_);
    s.TransactTime     = tt;
    for (int i = 0; i < 10; i++) {
        s.bid[i].Price = (int32_t)(atoll(f[15 + i * 4].c_str()) / pxDiv_);
        s.bid[i].Qty   = atoll(f[16 + i * 4].c_str()) * qtyMulSnap_;
        s.ask[i].Price = (int32_t)(atoll(f[17 + i * 4].c_str()) / pxDiv_);
        s.ask[i].Qty   = atoll(f[18 + i * 4].c_str()) * qtyMulSnap_;
    }
    Event ev;
    ev.type = MsgType_snap;
    ev.snap = s;
    return ev;
}

Event ClickHouseSource::parseIncLine(const std::string& line) {
    std::vector<std::string> f = splitTsv(line);
    // 统一 12 列: seq_no, timestamp, price, quantity, side, kind,
    //             order_type, order_orig_no, trade_type, bid_no, ask_no, biz_index
    // 字段数检查必须先于一切字段访问 (行被截断时越界读是 UB)
    if (f.size() < 12) throw std::runtime_error("逐笔单流行字段数不足: " + line.substr(0, 96));
    if (atoll(f[5].c_str()) == 1) {   // ORDER 行
        AxsbeOrder o{};
        o.secSrc = sse_ ? SecurityIDSource_SSE : SecurityIDSource_SZSE;
        o.securityID = securityId_;
        o.ChannelNo = 2013;
        o.Price        = atoll(f[2].c_str()) / pxDiv_;
        o.OrderQty     = atoll(f[3].c_str()) * qtyMulInc_;
        o.Side         = sideChar(atoll(f[4].c_str()), sse_);
        o.OrdType      = ordTypeChar(atoll(f[6].c_str()), sse_);
        o.TransactTime = tsToInt(f[1]);
        if (sse_) {
            // 委托映射键用订单号 (order_orig_no), 撤单与成交回链同一空间
            o.ApplSeqNum = strtoull(f[7].c_str(), nullptr, 10);
            o.OrderNo    = strtoull(f[7].c_str(), nullptr, 10);
            o.BizIndex   = strtoull(f[11].c_str(), nullptr, 10);
        } else {
            o.ApplSeqNum = strtoull(f[0].c_str(), nullptr, 10);
        }
        Event ev;
        ev.type = MsgType_order;
        ev.order = o;
        return ev;
    }
    // TRANSACTION 行
    AxsbeExe e{};
    e.secSrc = sse_ ? SecurityIDSource_SSE : SecurityIDSource_SZSE;
    e.securityID = securityId_;
    e.ChannelNo = 2013;
    e.BidApplSeqNum   = strtoull(f[9].c_str(), nullptr, 10);
    e.OfferApplSeqNum = strtoull(f[10].c_str(), nullptr, 10);
    e.LastPx     = atoll(f[2].c_str()) / pxDiv_;
    e.LastQty    = atoll(f[3].c_str()) * qtyMulInc_;
    e.TransactTime = tsToInt(f[1]);
    if (sse_) {
        e.ApplSeqNum = strtoull(f[11].c_str(), nullptr, 10);  // BizIndex 作序列
        e.BizIndex   = strtoull(f[11].c_str(), nullptr, 10);
        e.ExecType   = sideChar(atoll(f[4].c_str()), sse_);   // 'B'外盘/'S'内盘/'N'未知
    } else {
        bool isCancel = (atoll(f[8].c_str()) == 12355);
        e.ApplSeqNum = strtoull(f[0].c_str(), nullptr, 10);
        e.LastPx     = isCancel ? 0 : e.LastPx;
        e.ExecType   = isCancel ? (uint8_t)'4' : (uint8_t)'F';
    }
    Event ev;
    ev.type = MsgType_exe;
    ev.exe = e;
    return ev;
}

} // namespace source
