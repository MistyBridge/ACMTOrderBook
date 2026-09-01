#include "pipeline.h"
#include "../core/cpu_affinity.h"   // 定义 AXOB_HAS_STD_THREAD + setThreadAffinity 系列
#include <cstdio>
#include <chrono>

// =====================================================================
//  跨平台线程创建
//   - Linux/macOS/MSVC (posix 线程模型): 用 std::thread + setThreadAffinity
//   - MinGW win32 线程模型 (std::thread 不可用): 用 Win32 CreateThread
// =====================================================================

#if !defined(AXOB_HAS_STD_THREAD)
// ---- MinGW win32 线程模型专用路径 ----
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

struct ProducerArg {
    const char* dataFile;
    axob::core::SPSCQueue<MarketEvent>* queue;
    ProducerStats* stats;
    int replayCount;
};

struct ConsumerArg {
    axob::core::SPSCQueue<MarketEvent>* queue;
    AXOB* axob;
    axob::core::LatencyStats* latency;
    ConsumerStats* stats;
};

static DWORD WINAPI producerEntry(LPVOID arg) {
    auto* a = static_cast<ProducerArg*>(arg);
    producerThread(a->dataFile, *a->queue, *a->stats, a->replayCount);
    return 0;
}

static DWORD WINAPI consumerEntry(LPVOID arg) {
    auto* a = static_cast<ConsumerArg*>(arg);
    consumerThread(*a->queue, *a->axob, *a->latency, *a->stats);
    return 0;
}
#endif  // !AXOB_HAS_STD_THREAD

// =====================================================================
//  Pipeline 构造函数
// =====================================================================
Pipeline::Pipeline(const char* dataFile,
                   size_t queueCapacity,
                   size_t batchSize,
                   int producerCore,
                   int consumerCore,
                   int replayCount)
    : dataFile_(dataFile)
    , queueCapacity_(queueCapacity)
    , batchSize_(batchSize)
    , producerCore_(producerCore)
    , consumerCore_(consumerCore)
    , replayCount_(replayCount)
{}

// =====================================================================
//  Pipeline::run() — 启动管道，阻塞直到处理完毕
// =====================================================================
void Pipeline::run() {
    // 创建 SPSC 队列
    axob::core::SPSCQueue<MarketEvent> queue(queueCapacity_);

    // 创建 AXOB 实例 — 以 0/未知 起，由首条消息自动识别 securityID/secSrc (引擎通用)
    AXOB axob(0, SecurityIDSource_NULL, InstrumentType::UNKNOWN);

    // 创建延迟统计器
    axob::core::LatencyStats latency;

    // 创建统计结构体
    ProducerStats producerStats;
    ConsumerStats consumerStats;

    // 记录开始时间
    auto t0 = std::chrono::high_resolution_clock::now();

    // 启动生产者/消费者线程 + 绑核
#if defined(AXOB_HAS_STD_THREAD)
    // posix/MSVC: std::thread (天然跨平台)
    std::thread producer([&]() {
        producerThread(dataFile_, queue, producerStats, replayCount_);
    });
    std::thread consumer([&]() {
        consumerThread(queue, axob, latency, consumerStats);
    });
    axob::core::setThreadAffinity(producer, producerCore_);
    axob::core::setThreadAffinity(consumer, consumerCore_);
    producer.join();
    consumer.join();
#else
    // MinGW win32 线程模型: Win32 CreateThread
    ProducerArg pArg{dataFile_, &queue, &producerStats, replayCount_};
    ConsumerArg cArg{&queue, &axob, &latency, &consumerStats};
    HANDLE hProducer = CreateThread(nullptr, 0, producerEntry, &pArg, 0, nullptr);
    HANDLE hConsumer = CreateThread(nullptr, 0, consumerEntry, &cArg, 0, nullptr);
    axob::core::setThreadAffinityByHandle(hProducer, producerCore_);
    axob::core::setThreadAffinityByHandle(hConsumer, consumerCore_);
    WaitForSingleObject(hProducer, INFINITE);
    WaitForSingleObject(hConsumer, INFINITE);
    CloseHandle(hProducer);
    CloseHandle(hConsumer);
#endif

    // 记录结束时间
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    // 汇总统计到 Result
    result_.totalMsgs = consumerStats.orderCnt + consumerStats.exeCnt + consumerStats.snapCnt;
    result_.orderCnt = consumerStats.orderCnt;
    result_.exeCnt = consumerStats.exeCnt;
    result_.snapCnt = consumerStats.snapCnt;
    result_.elapsedSec = elapsed;
    result_.throughput = result_.totalMsgs / elapsed;          // T1 系统端到端 (参考)
    // T2 引擎纯处理吞吐: 全部真实消息数 * 1e9 / 全部 onMsg 耗时总和 (逐条全量, 与 Linux 基准一致)
    result_.engineComputeNs = consumerStats.engineComputeNs.load(std::memory_order_relaxed);
    result_.samplingCount   = consumerStats.samplingCount.load(std::memory_order_relaxed);
    if (result_.engineComputeNs > 0 && result_.samplingCount > 0) {
        result_.throughputEngine =
            (double)result_.samplingCount * 1e9 / (double)result_.engineComputeNs;
    }
    result_.latency = latency.snapshot();
    result_.producedTimeNs = producerStats.totalTimeNs.load(std::memory_order_relaxed);
    result_.consumedTimeNs = consumerStats.totalTimeNs.load(std::memory_order_relaxed);

    // 输出结果（兼容 Dashboard.exe 解析格式; 主指标为 T2 引擎纯处理吞吐 + L1 延迟）
    printf("\n=== Results ===\n");
    printf("Total: %d msgs (order=%d exe=%d snap=%d)\n",
           result_.totalMsgs, result_.orderCnt, result_.exeCnt, result_.snapCnt);
    // Time 行保持 dashboard.py 兼容格式 (T1 系统端到端); 引擎口径另起一行
    printf("Time:  %.3f s (%.0f msg/s)\n", result_.elapsedSec, result_.throughput);
    printf("Throughput(engine): %.0f msg/s (引擎纯处理 T2, 剔除I/O)\n",
           result_.throughputEngine);
    // Latency 行保持 dashboard.py 兼容 (p50/p99/p99.9/pmax), 值为 L1 单事件 onMsg 耗时
    printf("Latency: p50=%.1fus p99=%.1fus p99.9=%.1fus pmax=%.1fus (L1 单事件onMsg, 逐条全量, n=%llu/%d)\n",
           result_.latency.p50 / 1000.0,
           result_.latency.p99 / 1000.0,
           result_.latency.p999 / 1000.0,
           result_.latency.pmax / 1000.0,
           (unsigned long long)result_.samplingCount,
           result_.totalMsgs);

    printf("\nOrderBook State:\n%s\n", axob.toString().c_str());

    auto [askLevels, bidLevels] = axob.getLevels(5);
    printf("\n--- 5 Level OrderBook (internal ×10^6) ---\n");
    for (int i = 4; i >= 0; i--) {
        auto it = askLevels.find(i);
        if (it != askLevels.end() && it->second.qty > 0)
            printf("  Ask[%d]  %lld * %lld\n", i, (long long)it->second.price, (long long)it->second.qty);
    }
    printf("  -----\n");
    for (int i = 0; i < 5; i++) {
        auto it = bidLevels.find(i);
        if (it != bidLevels.end() && it->second.qty > 0)
            printf("  Bid[%d]  %lld * %lld\n", i, (long long)it->second.price, (long long)it->second.qty);
    }
    fflush(stdout);
}

// =====================================================================
//  Pipeline::getResult() — 获取结果
// =====================================================================
Pipeline::Result Pipeline::getResult() const {
    return result_;
}
