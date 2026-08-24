#pragma once
#include <atomic>
#include "../core/spsc_queue.h"
#include "event.h"

// =====================================================================
//  ProducerStats — 生产者统计
// =====================================================================
struct ProducerStats {
    alignas(64) std::atomic<uint64_t> totalProduced{0};
    alignas(64) std::atomic<uint64_t> totalTimeNs{0};
};

// =====================================================================
//  producerThread — 生产者线程函数
//  读取数据文件，解析消息，推入 SPSC 队列
//  replayCount: 重放次数，用于放大测试数据集
// =====================================================================
void producerThread(const char* dataFile, axob::core::SPSCQueue<MarketEvent>& queue, ProducerStats& stats, int replayCount = 1);
