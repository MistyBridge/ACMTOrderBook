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
