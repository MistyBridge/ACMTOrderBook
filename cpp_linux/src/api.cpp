// api.cpp — C API 实现: 包装 AXOB 引擎 (深交所) + 快照校验闭环
#include "acmt_orderbook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <algorithm>
#include <vector>
#include <chrono>

#include "../cpp v2/behave/axob.h"
#include "../cpp v2/source/clickhouse_source.h"

// 统一口径 (引擎基准): 一律用单调时钟 steady_clock 计时。
namespace {
using ObClock = std::chrono::steady_clock;
inline uint64_t ob_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        ObClock::now().time_since_epoch()).count();
}
} // namespace

namespace {

struct ObHandle {
    AXOB engine;
    int64_t eventCount = 0;
    int64_t orderCount = 0;
    int64_t tradeCount = 0;

    // ---- 快照校验 (num_trades 对齐等待式匹配, 与 ch_replay 同口径) ----
    std::map<int64_t, std::vector<AxsbeSnapStock>> pending;
    acmt_ob_validation_t val{};

    // ---- 1s 聚合校验 (引擎累计口径 vs 输入口径, 每秒边界核对) ----
    // 引擎独立维护 NumTrades/TotalVolumeTrade/TotalValueTrade/LastPx,
    // 在秒边界与输入行的累计值核对: 丢笔/重笔/口径漂移都会在此暴露。
    int64_t barSod = -1;                     // 当前秒 HHMMSS
    int64_t truthCnt = 0, truthVol = 0, truthTurnoverInter = 0;   // Inter: 内部 ×10^5 域
    int32_t truthClosePx = 0;                // 末笔价 (原始精度: 深 ×10^4 / 沪 ×10^3)

    void barTick(const AxsbeExe& e) {
        int64_t sod = (int64_t)((e.TransactTime % 1000000000ULL) / 1000);
        if (barSod == -1) barSod = sod;
        else if (sod > barSod) { finalizeBar(); barSod = sod; }
        truthCnt++;
        truthVol += e.LastQty;
        // 精度对齐交易所原生 (深: 价×10^4/量×10^2, 沪: 价×10^3/量×10^3), 金额 = 价格×数量,
        // 除以 per-exchange 因子落到交易所金额精度 — 与 axob_trade.cpp + genSnap 一致。
        // 乘积精度 ×10^6, 对境内单笔 (≈1e11 元) 仅 ~1e17, 远低于 int64 上限, 无需 __int128。
        int64_t pxInter  = (e.secSrc == SecurityIDSource_SZSE)
                               ? e.LastPx * SZSE_PRICE_MUL
                               : e.LastPx * SSE_PRICE_MUL;
        int64_t qtyInter = qtySnap2Inter(e.LastQty, e.secSrc);
        int64_t amtInter = amtFromProd(pxInter, qtyInter, e.secSrc);
        // 与 eAmt 同域比较: 累加内部精度, 到秒边界再整体换算 (避免逐笔换算的舍入累积)
        truthTurnoverInter += amtInter;
        truthClosePx = (int32_t)e.LastPx;
    }
    void finalizeBar() {
        if (barSod == -1) return;
        val.bar_seconds++;
        // 比对域统一为"原始精度": 引擎侧内部 ×10^5 一律换算回原始, truth 侧本就是原始。
        int64_t eCnt   = engine.NumTrades;
        int64_t eVol   = qtyInter2Snap(engine.TotalVolumeTrade, engine.secSrc);
        int64_t eAmt   = amtInter2Snap(engine.TotalValueTrade, engine.secSrc);
        // truth 侧同样换算到原始精度域后再比 (两侧同域是比对成立的前提)
        int64_t truthAmt = amtInter2Snap(truthTurnoverInter, engine.secSrc);
        int64_t eClose = fmtPriceInter2Snap(engine.LastPx, engine.instType, engine.secSrc);
        // close 仅比对连续竞价窗口: 收盘后引擎按设计用官方收盘价(快照)覆写
        // LastPx, 与逐笔流最后成交价是不同口径 (竞价/盘后同理)。
        bool closeComparable = (barSod >= 93000 && barSod < 113000) ||
                               (barSod >= 130000 && barSod < 145700);
        if (eCnt != truthCnt || eVol != truthVol ||
            eAmt != truthAmt ||
            (closeComparable && eClose != truthClosePx)) {
            val.bar_mismatch++;
            if (eVol != truthVol) val.bar_vol_mismatch++;
            if (val.bar_mismatch <= 5) {   // TEMP DEBUG
                fprintf(stderr, "BARDBG sod=%lld cnt=%lld/%lld vol=%lld/%lld amt=%lld/%lld close=%lld/%d\n",
                        (long long)barSod, (long long)eCnt, (long long)truthCnt,
                        (long long)eVol, (long long)truthVol,
                        (long long)eAmt, (long long)truthAmt,
                        (long long)eClose, truthClosePx);
            }
        }
    }

    explicit ObHandle(const char* inst, int exchange)
        : engine(std::stoi(std::string(inst).substr(0, 6)),
                 exchange == 2 ? SecurityIDSource_SZSE : SecurityIDSource_SSE,
                 InstrumentType::STOCK) {}

    static int snapPhase(const AxsbeSnapStock& s) {
        auto t = s.tradingPhaseMarket();
        if (t == TPM::OpenCall)  return 1;
        if (t == TPM::CloseCall) return 2;
        return 0;
    }

    // 单次对比: 返回是否全等。全等匹配(在途)与最终分类(finalize)均即时计数。
    bool tryMatch(const AxsbeSnapStock& s, bool finalize) {
        int ph = snapPhase(s);
        if (ph == 2) {   // 收盘竞价: 仅比 last
            AxsbeSnapStock gen = engine.genTradingSnap(false, ACMT_DEPTH);
            bool ok = gen.LastPx == s.LastPx;
            if (ok) { val.call_total++; val.call_full_exact++; }
            else if (finalize) { val.call_total++; val.call_mismatch++; }
            return ok;
        }
        // 路由感知: 兜底开启且处于快照模式时, 校验比对的也是路由输出 (套B)
        AxsbeSnapStock gen = (ph == 1 && !engine.snapFallbackEnabled)
                                 ? engine.genCallSnap(ACMT_DEPTH)
                                 : engine.currentBook(ACMT_DEPTH);
        bool statsOK = gen.NumTrades == s.NumTrades &&
                       gen.TotalVolumeTrade == s.TotalVolumeTrade &&
                       gen.LastPx == s.LastPx;
        // 上交所竞价快照的 tag31 在竞价期天然为 0 (规范 UA3202 说明第 11(2) 条),
        // 与深交所(发布虚拟撮合价)约定不同, SSE 竞价校验不比 last。
        if (ph == 1 && engine.secSrc == SecurityIDSource_SSE) {
            statsOK = gen.NumTrades == s.NumTrades &&
                      gen.TotalVolumeTrade == s.TotalVolumeTrade;
        }
        int64_t lvlDiff = 0;
        for (int k = 0; k < ACMT_DEPTH; k++) {
            if (!(gen.bid[k] == s.bid[k])) lvlDiff++;
            if (!(gen.ask[k] == s.ask[k])) lvlDiff++;
        }
        bool fullOK = statsOK && lvlDiff == 0;
        if (fullOK) {   // 匹配即计数
            if (ph == 0) {
                val.trading_total++;
                val.trading_full_exact++;
                val.trading_avg_level_match = (val.trading_avg_level_match * (val.trading_total - 1)
                                               + (ACMT_DEPTH * 2)) / val.trading_total;
            } else {
                val.call_total++;
                val.call_full_exact++;
            }
            return true;
        }
        if (finalize) {   // 笔数已越过键: 最终分类
            if (ph == 0) {
                val.trading_total++;
                val.trading_avg_level_match = (val.trading_avg_level_match * (val.trading_total - 1)
                                               + (ACMT_DEPTH * 2 - lvlDiff)) / val.trading_total;
                if (statsOK) val.trading_stats_only++;
                else val.trading_mismatch++;
            } else {
                val.call_total++;
                val.call_mismatch++;
            }
        }
        if (finalize && ph == 0 && val.trading_mismatch <= 3) {
            fprintf(stderr, "DBG mis#%lld t=%llu mkt(nt=%lld vol=%lld last=%d b0=%d*%lld a0=%d*%lld b1=%d*%lld a1=%d*%lld b9=%d*%lld a9=%d*%lld) gen(nt=%lld vol=%lld last=%d b0=%d*%lld a0=%d*%lld b1=%d*%lld a1=%d*%lld b9=%d*%lld a9=%d*%lld)\n",
                    (long long)val.trading_mismatch + 1,
                    (unsigned long long)s.TransactTime,
                    (long long)s.NumTrades, (long long)s.TotalVolumeTrade, s.LastPx,
                    s.bid[0].Price, (long long)s.bid[0].Qty, s.ask[0].Price, (long long)s.ask[0].Qty,
                    s.bid[1].Price, (long long)s.bid[1].Qty, s.ask[1].Price, (long long)s.ask[1].Qty,
                    s.bid[9].Price, (long long)s.bid[9].Qty, s.ask[9].Price, (long long)s.ask[9].Qty,
                    (long long)gen.NumTrades, (long long)gen.TotalVolumeTrade, gen.LastPx,
                    gen.bid[0].Price, (long long)gen.bid[0].Qty, gen.ask[0].Price, (long long)gen.ask[0].Qty,
                    gen.bid[1].Price, (long long)gen.bid[1].Qty, gen.ask[1].Price, (long long)gen.ask[1].Qty,
                    gen.bid[9].Price, (long long)gen.bid[9].Qty, gen.ask[9].Price, (long long)gen.ask[9].Qty);
        }
        return false;
    }

    void matchPending() {
        if (pending.empty()) return;
        int64_t nt = engine.NumTrades;
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->first + 2 < nt) {   // 越过窗口: 最终分类 (一次性全量)
                for (auto& s : it->second) tryMatch(s, true);
                it = pending.erase(it);
                continue;
            }
            if (it->first <= nt) {      // 窗口内: 尝试全等匹配
                auto& vec = it->second;
                // [史] 曾用 key-0 队首制 (只试 vec[0], 假设集合竞价盘口单调):
                // SSE 盘口非单调 (撤单) 时队首未命中的帧卡住后续帧 — 600584
                // call 仅 61/260 vs 基线 254/260。恢复基线全量试 (逐帧比对)。
                for (size_t i = 0; i < vec.size();) {
                    if (tryMatch(vec[i], false)) vec.erase(vec.begin() + i);
                    else i++;
                }
                if (vec.empty()) it = pending.erase(it);
                else ++it;
            } else {
                break;   // 未来键: 键单调递增, 后面全在未来, 无需遍历
            }
        }
    }

    void afterEvent() { matchPending(); }

    void pushSnap(const AxsbeSnapStock& s) {
        auto t = s.tradingPhaseMarket();
        if (t == TPM::AMTrading || t == TPM::PMTrading ||
            t == TPM::OpenCall || t == TPM::CloseCall) {
            pending[s.NumTrades].push_back(s);
        }
    }
};

inline ObHandle* H(void* h) { return static_cast<ObHandle*>(h); }

void fillSnap(const AxsbeSnapStock& s, acmt_snap_t* out) {
    memset(out, 0, sizeof(*out));
    out->transact_time       = s.TransactTime;
    out->num_trades          = s.NumTrades;
    out->total_volume_trade  = s.TotalVolumeTrade;
    out->total_value_trade   = s.TotalValueTrade;
    out->last_px             = s.LastPx;
    out->open_px             = s.OpenPx;
    out->high_px             = s.HighPx;
    out->low_px              = s.LowPx;
    out->prev_close_px       = s.PrevClosePx;
    out->upper_limit_px      = s.UpLimitPx;
    out->lower_limit_px      = s.DnLimitPx;
    out->total_bid_vol       = s.BidWeightSize;
    out->total_ask_vol       = s.AskWeightSize;
    for (int i = 0; i < ACMT_DEPTH; i++) {
        out->bid_price[i]  = s.bid[i].Price;
        out->bid_volume[i] = s.bid[i].Qty;
        out->ask_price[i]  = s.ask[i].Price;
        out->ask_volume[i] = s.ask[i].Qty;
    }
}

AxsbeSnapStock fromApi(const acmt_snap_t* in) {
    AxsbeSnapStock s{};
    s.secSrc = SecurityIDSource_SZSE;
    s.securityID = 1;
    s.ChannelNo = 2013;
    s.TransactTime      = in->transact_time;
    s.NumTrades         = in->num_trades;
    s.TotalVolumeTrade  = in->total_volume_trade;
    s.TotalValueTrade   = in->total_value_trade;
    s.LastPx            = in->last_px;
    s.OpenPx            = in->open_px;
    s.HighPx            = in->high_px;
    s.LowPx             = in->low_px;
    s.PrevClosePx       = in->prev_close_px;
    s.UpLimitPx         = in->upper_limit_px;
    s.DnLimitPx         = in->lower_limit_px;
    s.BidWeightSize     = in->total_bid_vol;
    s.AskWeightSize     = in->total_ask_vol;
    for (int i = 0; i < ACMT_DEPTH; i++) {
        s.bid[i].Price = in->bid_price[i];
        s.bid[i].Qty   = in->bid_volume[i];
        s.ask[i].Price = in->ask_price[i];
        s.ask[i].Qty   = in->ask_volume[i];
    }
    return s;
}

} // namespace

extern "C" {

acmt_ob_handle acmt_ob_create(const char* instrument, int exchange) {
    try {
        return new ObHandle(instrument, exchange);
    } catch (...) {
        return nullptr;
    }
}

void acmt_ob_destroy(acmt_ob_handle h) {
    delete H(h);
}

void acmt_ob_on_order(acmt_ob_handle h, uint64_t seq,
                      int64_t price, int64_t qty,
                      char side, char ord_type, uint64_t transact_time) {
    ObHandle* ob = H(h);
    AxsbeOrder o{};
    o.secSrc = SecurityIDSource_SZSE;
    o.securityID = 1;
    o.ChannelNo = 2013;
    o.ApplSeqNum   = seq;
    o.Price        = price;
    o.OrderQty     = qty;
    o.Side         = (uint8_t)side;
    o.OrdType      = (uint8_t)ord_type;
    o.TransactTime = transact_time;
    ob->engine.onMsg(o);
    ob->eventCount++;
    ob->orderCount++;
    ob->afterEvent();
}

void acmt_ob_on_exec(acmt_ob_handle h, uint64_t seq,
                     uint64_t bid_seq, uint64_t ask_seq,
                     int64_t price, int64_t qty, char exec_type,
                     uint64_t transact_time) {
    ObHandle* ob = H(h);
    AxsbeExe e{};
    e.secSrc = SecurityIDSource_SZSE;
    e.securityID = 1;
    e.ChannelNo = 2013;
    e.ApplSeqNum     = seq;
    e.BidApplSeqNum  = bid_seq;
    e.OfferApplSeqNum = ask_seq;
    e.LastPx         = price;
    e.LastQty        = qty;
    e.ExecType       = (uint8_t)exec_type;
    e.TransactTime   = transact_time;
    ob->engine.onMsg(e);
    ob->eventCount++;
    if (exec_type == 'F') ob->tradeCount++;
    ob->afterEvent();
}

void acmt_ob_on_snap(acmt_ob_handle h, const acmt_snap_t* snap) {
    ObHandle* ob = H(h);
    AxsbeSnapStock s = fromApi(snap);
    ob->pushSnap(s);
    ob->engine.onMsg(s);
    ob->eventCount++;
    ob->afterEvent();
}

static int64_t replayChImpl(acmt_ob_handle h,
                            const char* host, int port,
                            const char* user, const char* password,
                            const char* date, const char* instrument,
                            int exchange, bool validate,
                            int skipSod, int skipSec) {
    ObHandle* ob = H(h);
    try {
        auto tL0 = ObClock::now();
        source::ClickHouseSource src(host, port, user, password);
        src.load(date, instrument, exchange);   // 仅建立三路流式查询 (毫秒级)
        auto tL1 = ObClock::now();
        AxsbeOrder ord; AxsbeExe exe; AxsbeSnapStock snap;
        int64_t n = 0;
        // L1 延迟 (bench 模式, 与 Linux 基准口径一致): 每条真实消息都掐表其单事件 onMsg 处理耗时 (ns)
        std::vector<int64_t> latNs;
        ObClock::duration fetchS(0);   // hasNext 中的阻塞拉取+解析
        while (true) {
            auto tF0 = ObClock::now();
            bool more = src.hasNext();
            auto tF1 = ObClock::now();
            fetchS += tF1 - tF0;
            if (!more) break;
            int type = src.next(ord, exe, snap);
            // 丢单模拟: [skipSod, skipSod+skipSec) 秒内丢弃逐笔 (快照照喂)
            if (skipSec > 0 && type != MsgType_snap) {
                uint64_t tt = (type == MsgType_order) ? ord.TransactTime : exe.TransactTime;
                int64_t sod = (int64_t)((tt % 1000000000ULL) / 1000);
                if (sod >= skipSod && sod < (int64_t)skipSod + skipSec) continue;
            }
            // 逐条全量计时 (与 Linux 基准口径一致): bench 模式下每条真实消息都掐表
            bool isReal = (type == MsgType_order || type == MsgType_exe || type == MsgType_snap);
            bool doTime = (isReal && !validate);
            if (type == MsgType_order) {
                uint64_t t0m = doTime ? ob_now_ns() : 0;
                ob->engine.onMsg(ord);
                if (doTime) latNs.push_back(ob_now_ns() - t0m);
                ob->orderCount++;
                if (validate) ob->afterEvent();
            } else if (type == MsgType_exe) {
                bool isTrade = (exe.ExecType == 'F' ||
                                ob->engine.secSrc == SecurityIDSource_SSE);
                if (isTrade) {
                    if (validate) ob->barTick(exe);
                    ob->tradeCount++;
                }
                uint64_t t0m = doTime ? ob_now_ns() : 0;
                ob->engine.onMsg(exe);
                if (doTime) latNs.push_back(ob_now_ns() - t0m);
                if (validate) ob->afterEvent();
            } else if (type == MsgType_snap) {
                if (validate) ob->pushSnap(snap);
                uint64_t t0m = doTime ? ob_now_ns() : 0;
                ob->engine.onMsg(snap);
                if (doTime) latNs.push_back(ob_now_ns() - t0m);
                if (validate) ob->afterEvent();
            } else {
                continue;
            }
            ob->eventCount++;
            n++;
        }
        // 收尾: 结算最后一个 1s bar + 残余待匹配快照最终分类
        if (validate) {
            ob->finalizeBar();
            for (auto& kv : ob->pending)
                for (auto& s : kv.second) ob->tryMatch(s, true);
            ob->pending.clear();
        }
        auto tR = ObClock::now();
        double loadS   = std::chrono::duration<double>(tL1 - tL0).count();
        double wallS   = std::chrono::duration<double>(tR - tL1).count();
        double fetchSec = std::chrono::duration<double>(fetchS).count();
        double replayS = wallS - fetchSec;   // 墙钟回放(含拉取) - 拉取 = 引擎侧耗时 (参考)
        if (!validate && !latNs.empty()) {
            // T2 引擎纯处理吞吐 = 全部真实消息数 * 1e9 / 全部 onMsg 耗时总和 (逐条全量, 与 Linux 基准一致)
            uint64_t sum = 0;
            for (auto v : latNs) sum += (uint64_t)v;
            std::sort(latNs.begin(), latNs.end());
            size_t k = latNs.size();
            auto pct = [&](double p) { return (double)latNs[(size_t)(p * (k - 1))]; };
            double engineTput = sum ? (double)k * 1e9 / (double)sum : 0.0;
            fprintf(stderr, "LAT n=%zu p50=%.1f p99=%.1f p99.9=%.1f pmax=%.1f (ns/msg)\n",
                    k, pct(0.50), pct(0.99), pct(0.999), pct(1.0));
            fprintf(stderr,
                    "REPLAYSTAT load=%.6fs fetch=%.6fs wall=%.6fs msgs=%lld engine=%.0f msg/s (纯处理)  wallRef=%.0f msg/s (含拉取/校验)\n",
                    loadS, fetchSec, wallS, (long long)n, engineTput,
                    wallS > 0 ? n / wallS : 0.0);
        } else {
            fprintf(stderr, "REPLAYSTAT load=%.6fs fetch=%.6fs replay=%.6fs msgs=%lld (%.1f msg/s 含校验)\n",
                    loadS, fetchSec, replayS, (long long)n,
                    replayS > 0 ? n / replayS : 0.0);
        }
        return n;
    } catch (const std::exception& e) {
        fprintf(stderr, "replay_ch error: %s\n", e.what());
        return -1;
    } catch (...) {
        fprintf(stderr, "replay_ch error: unknown\n");
        return -1;
    }
}

int64_t acmt_ob_replay_ch(acmt_ob_handle h,
                          const char* host, int port,
                          const char* user, const char* password,
                          const char* date, const char* instrument,
                          int exchange) {
    return replayChImpl(h, host, port, user, password, date, instrument, exchange, true, 0, 0);
}

int64_t acmt_ob_replay_ch_bench(acmt_ob_handle h,
                                const char* host, int port,
                                const char* user, const char* password,
                                const char* date, const char* instrument,
                                int exchange) {
    return replayChImpl(h, host, port, user, password, date, instrument, exchange, false, 0, 0);
}

int64_t acmt_ob_replay_ch_sim(acmt_ob_handle h,
                              const char* host, int port,
                              const char* user, const char* password,
                              const char* date, const char* instrument,
                              int exchange,
                              int fallback, int skipSod, int skipSec) {
    ObHandle* ob = H(h);
    ob->engine.snapFallbackEnabled = (fallback != 0);
    return replayChImpl(h, host, port, user, password, date, instrument, exchange,
                        true, skipSod, skipSec);
}

void acmt_ob_set_fallback(acmt_ob_handle h, int enable) {
    H(h)->engine.snapFallbackEnabled = (enable != 0);
}

void acmt_ob_set_order_cleanup(acmt_ob_handle h, int enable) {
    H(h)->engine.orderMapCleanupEnabled = (enable != 0);
}

void acmt_ob_set_stale_filter(acmt_ob_handle h, int enable) {
    H(h)->engine.staleDataFilterEnabled = (enable != 0);
}

int64_t acmt_ob_get_levels(acmt_ob_handle h,
                           acmt_level_t* asks, acmt_level_t* bids,
                           int max_levels) {
    ObHandle* ob = H(h);
    auto [askMap, bidMap] = ob->engine.getLevels(max_levels);
    const auto src = ob->engine.secSrc;
    const auto ity = ob->engine.instType;
    // 内部 ×10^5 → 对外快照原始精度 (与 acmt_snap_t 各档口径一致)
    int64_t i = 0;
    for (const auto& [p, l] : askMap) {
        if (i >= max_levels) break;
        asks[i].price = fmtPriceInter2Snap(l.price, ity, src);
        asks[i].qty   = qtyInter2Snap(l.qty, src);
        i++;
    }
    int64_t j = 0;
    for (const auto& [p, l] : bidMap) {
        if (j >= max_levels) break;
        bids[j].price = fmtPriceInter2Snap(l.price, ity, src);
        bids[j].qty   = qtyInter2Snap(l.qty, src);
        j++;
    }
    return (i > j) ? i : j;
}

void acmt_ob_get_health(acmt_ob_handle h, acmt_ob_health_t* out) {
    const auto& hl = H(h)->engine.health;
    out->order_not_found  = hl.orderNotFound;
    out->neg_level_clear  = hl.negLevelClear;
    out->snap_route_adopt = hl.snapRouteAdopt;
    out->cleanup_erased   = hl.cleanupErased;
    out->stale_filtered   = H(h)->engine.staleFiltered;
}

void acmt_ob_get_book(acmt_ob_handle h, acmt_snap_t* out) {
    ObHandle* ob = H(h);
    AxsbeSnapStock gen = ob->engine.genTradingSnap(false, ACMT_DEPTH);
    fillSnap(gen, out);
}

void acmt_ob_get_stat(acmt_ob_handle h, acmt_ob_stat_t* out) {
    ObHandle* ob = H(h);
    memset(out, 0, sizeof(*out));
    const auto src = ob->engine.secSrc;
    const auto ity = ob->engine.instType;
    out->num_trades         = ob->engine.NumTrades;
    // [基线对齐] 引擎内部统一 ×10^5 → 基线引擎域 (基线 fetchIncs 直接累计:
    //   深 ×10^4 / 沪 ×10^5, 金额 深 ×10^4 / 沪 ×10^7)。实测 70 场基线 CSV
    //   差值: 深 vol = 内部/10, 沪 vol = 内部; 深 amt = 内部×10, 沪 amt = 内部×100。
    const bool isSzse = (ob->engine.secSrc == SecurityIDSource_SZSE);
    out->total_volume_trade = isSzse ? ob->engine.TotalVolumeTrade / 10
                                     : ob->engine.TotalVolumeTrade;
    out->total_value_trade  = isSzse ? ob->engine.TotalValueTrade * 10
                                     : ob->engine.TotalValueTrade * 100;
    out->last_px            = fmtPriceInter2Snap(ob->engine.LastPx, ity, src);
    out->open_px            = fmtPriceInter2Snap(ob->engine.OpenPx, ity, src);
    out->high_px            = fmtPriceInter2Snap(ob->engine.HighPx, ity, src);
    out->low_px             = fmtPriceInter2Snap(ob->engine.LowPx, ity, src);
    out->event_count        = ob->eventCount;
    out->order_count        = ob->orderCount;
    out->trade_count        = ob->tradeCount;
}

void acmt_ob_get_validation(acmt_ob_handle h, acmt_ob_validation_t* out) {
    *out = H(h)->val;
}

} // extern "C"
