// clickhouse_source.h — ClickHouse 数据源: 拉取单只股票一个交易日的 L2 数据,
// 转换为内部消息 (AxsbeOrder/AxsbeExe/AxsbeSnapStock), 按回放顺序归并。
//
// 支持深交所 (exchange=2) 与上交所 (exchange=1) 股票。
// 精度层级 (引擎内部 ×10^5 定点归一化与此无关, 见 axob_*):
//   CH 原始值域: 深 价格×10^4 数量×10^2(百股) 金额×10^4; 沪 三表价格统一×10^4
//   数量×10^2(百股) 金额×10^4(实测 600584 20260716; 数量经拍卖单核对, 非股);
//   解析时经 pxDiv_/qtyMul_/amtMul_ 换算回交易所原生精度 (深=CH 原值; 沪 价格÷10,
//   数量×10 → ×10^3 域, 金额×10 → ×10^5 域), 进引擎后再归一化到内部统一 ×10^5。
//   时间 YYYYMMDDHHMMSSsss (北京时间)。
//
// 架构 (2026-08-17 按 LMAX Disruptor 理念重做, 经用户确认):
//   SPSC — 一个标的 = 一条独立流水线, 通道只允许单生产者单消费者。
//   - 生产者 (单线程): 拉取该标的「逐笔单流」— ORDER+TRANSACTION 在服务端
//     UNION ALL 并按序列键排序 (深: seq_no 全局唯一; 沪: biz_index 跨表共享,
//     已数据验证与时间戳序等价且同毫秒内唯一裁决) — 一个 QueryReader 顺序读,
//     不再有客户端两路归并; 快照 (TICK) 整拉驻留内存 (98KB 级), 按 TransactTime
//     插在「时间 ≤ 快照时间的逐笔前缀」之后 (与全量排序版同一语义)。
//   - 通道: SpscQueue (无锁环形队列), 生产者推、消费者取, 无仲裁。
//   - 消费者: 订单簿引擎侧, 经 hasNext()/next() 顺序取, 无需任何跨路比较。
//   旧实现 (三路并发流 + 客户端归并) 的慢流饿死快流 / 服务端 30s 断连问题
//   从此在结构上不存在: 数据侧已合成单流, 生产者只面对一个有序输入。
//
// 归并语义 (与全量排序版逐位一致): 逐笔按业务序列序; 每帧快照排在「时间戳 ≤
// 快照时间戳的逐笔前缀」之后。快照流按时间升序, 且逐笔序列序与时间序一致
// (数据验证: 按序列键排序时 timestamp 逆序数 = 0), 故只需缓存逐笔队首即可
// 判定快照插入点。
#pragma once
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

#include "../tool/axsbe_base.h"
#include "../tool/axsbe_order.h"
#include "../tool/axsbe_exe.h"
#include "../tool/axsbe_snap_stock.h"
#include "ch_client.h"
#include "spsc_queue.h"

namespace source {

struct Event {
    int type;                // MsgType_order / MsgType_exe / MsgType_snap
    AxsbeOrder     order;
    AxsbeExe       exe;
    AxsbeSnapStock snap;
};

class ClickHouseSource {
public:
    ClickHouseSource(const std::string& host, int port,
                     const std::string& user, const std::string& password);
    ~ClickHouseSource();     // 停生产者线程

    // 打开查询并启动生产者线程 (date: yyyymmdd; instrument: 6位代码;
    // exchange: 1=SSE 2=SZSE)。快照在此同步整拉驻留 (秒级); 逐笔单流流式拉取。
    void load(const std::string& date, const std::string& instrument, int exchange);

    // 消费者侧 (订单簿引擎): 从 SPSC 通道顺序取事件; 队列空时等待生产者。
    bool hasNext();
    int next(AxsbeOrder& o, AxsbeExe& e, AxsbeSnapStock& s);

    int64_t snapCount()  const { return snapCnt_; }
    int64_t orderCount() const { return orderCnt_; }
    int64_t exeCount()   const { return exeCnt_; }
    int64_t eventCount() const { return snapCnt_ + orderCnt_ + exeCnt_; }

private:
    // 行解析 (TSV 行 -> 事件; 逻辑与全量版逐行一致)
    Event parseSnapLine(const std::string& line);   // TICK 55 列
    Event parseIncLine(const std::string& line);    // 逐笔单流 12 列 (kind 区分)

    // 生产者线程: 逐笔单流顺序读, 快照按时间插入, 推入 SPSC
    void producerLoop();
    void stopProducer();

    // 快照整拉 (load 内同步执行): TICK 查询一次读完驻留内存
    std::vector<Event> snaps_;      // 按时间升序
    size_t snapPos_ = 0;            // 已发出快照位置
    bool snapAll_ = false;          // 快照全部发出

    // 逐笔队首 (生产者线程私有)
    bool hasInc_ = false;
    bool incDone_ = false;      // 逐笔单流已尽
    Event incLook_;

    // 生产者完成标志 (消费者读取; 控制面小锁, 数据通道仍为无锁 SPSC)
    std::mutex doneM_;
    bool prodDone_ = false;
    std::string prodError_;

    std::string host_;
    int port_;
    std::string user_, pass_;
    int securityId_ = 0;
    bool sse_ = false;
    int64_t pxDiv_ = 1, qtyMulInc_ = 1, qtyMulSnap_ = 1, amtMul_ = 1;
    // CH 原始值域 -> 交易所原生精度; 沪三表价格统一 ×10^4 (实测), pxDiv_ 沪=10;
    // 数量: 逐笔 深百股/沪股, 快照深沪均为百股 (实测)

    // [v3.3] 逐笔单流改为批量全量拉取 (query 物化驻留): 流式 QueryReader 挂
    // CH 长连接, 队列满背压时 CH send 阻塞超时断连 (600584 177 万行实测
    // 截断); 基线 (全量 query) 从未失败。139MB 级内存驻留可接受。
    std::string incBody_;        // 全量逐笔 TSV 正文
    size_t incPos_ = 0;          // 已消费行偏移
    std::thread producerTh_;
    std::atomic<bool> stop_{false};

    // SPSC 通道: 生产者线程 push, 消费者 (hasNext/next) pop
    SpscQueue<Event> q_{65536};

    // 消费者侧: 批取缓冲 (Disruptor 批处理理念 — 一次取空队列到本地,
    // 每批只经历一次等待, 避免逐事件 yield 在负载高的机器上退化)
    static constexpr size_t CONSUMER_BATCH = 1024;
    Event batch_[CONSUMER_BATCH];
    size_t batchPos_ = 0, batchCnt_ = 0;
    bool hasReady_ = false;
    Event readyEv_;     // 当前事件 (取自批缓冲)

    int64_t snapCnt_ = 0, orderCnt_ = 0, exeCnt_ = 0;
    bool loaded_ = false;
};

} // namespace source
