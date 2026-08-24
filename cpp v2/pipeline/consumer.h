#pragma once
#include <atomic>
#include "../core/spsc_queue.h"
#include "../core/latency_stats.h"
#include "../behave/axob.h"
#include "event.h"

// =====================================================================
//  ConsumerStats — 消费者统计
// =====================================================================
struct ConsumerStats {
    alignas(64) std::atomic<uint64_t> totalConsumed{0};
    alignas(64) std::atomic<uint64_t> totalTimeNs{0};
    // ---- 引擎纯处理口径 (T2, 与 Linux 基准一致): 逐条全量 onMsg 耗时累加 与 条数 ----
    // 吞吐 T2 = 全部真实消息数 * 1e9 / 全部 onMsg 耗时总和 (引擎纯处理速率, 剔除 I/O)
    alignas(64) std::atomic<uint64_t> engineComputeNs{0};   // 全部 onMsg 耗时总和 (ns)
    std::atomic<uint64_t> samplingCount{0};                 // 计入的真实消息条数 (= totalMsgs)
    int orderCnt = 0;
    int exeCnt   = 0;
    int snapCnt  = 0;
};

// =====================================================================
//  consumerThread — 消费者线程函数
//  从 SPSC 队列批量取出消息，分发到 AXOB 处理
// =====================================================================
void consumerThread(axob::core::SPSCQueue<MarketEvent>& queue, AXOB& axob,
                    axob::core::LatencyStats& latency, ConsumerStats& stats);
