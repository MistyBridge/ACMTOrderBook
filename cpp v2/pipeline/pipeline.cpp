#include "pipeline.h"
#include "../core/cpu_affinity.h"
#include <cstdio>
#include <chrono>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

// =====================================================================
//  跨平台线程创建（MinGW win32 线程模型不支持 std::thread）
// =====================================================================

struct ProducerArg {
    const char* dataFile;
    axob::core::SPSCQueue<MarketEvent>* queue;
    ProducerStats* stats;
    int replayCount;  // 重放次数
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

    // 创建 AXOB 实例（与 v1 一致）
    AXOB axob(1, SecurityIDSource_SZSE, InstrumentType::STOCK);

    // 创建延迟统计器
    axob::core::LatencyStats latency;

    // 创建统计结构体
    ProducerStats producerStats;
    ConsumerStats consumerStats;

    // 线程参数（栈上，生命周期覆盖线程运行期间）
    ProducerArg pArg{dataFile_, &queue, &producerStats, replayCount_};
    ConsumerArg cArg{&queue, &axob, &latency, &consumerStats};

    // 记录开始时间
    auto t0 = std::chrono::high_resolution_clock::now();

    // 启动生产者线程
    HANDLE hProducer = CreateThread(
        nullptr, 0, producerEntry, &pArg, 0, nullptr);

    // 启动消费者线程
    HANDLE hConsumer = CreateThread(
        nullptr, 0, consumerEntry, &cArg, 0, nullptr);

    // 绑定 CPU 核心
    axob::core::setThreadAffinityByHandle(hProducer, producerCore_);
    axob::core::setThreadAffinityByHandle(hConsumer, consumerCore_);

    // 等待两个线程完成
    WaitForSingleObject(hProducer, INFINITE);
    WaitForSingleObject(hConsumer, INFINITE);
    CloseHandle(hProducer);
    CloseHandle(hConsumer);

    // 记录结束时间
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    // 汇总统计到 Result
    result_.totalMsgs = consumerStats.orderCnt + consumerStats.exeCnt + consumerStats.snapCnt;
    result_.orderCnt = consumerStats.orderCnt;
    result_.exeCnt = consumerStats.exeCnt;
    result_.snapCnt = consumerStats.snapCnt;
    result_.elapsedSec = elapsed;
    result_.throughput = result_.totalMsgs / elapsed;
    result_.latency = latency.snapshot();
    result_.producedTimeNs = producerStats.totalTimeNs.load(std::memory_order_relaxed);
    result_.consumedTimeNs = consumerStats.totalTimeNs.load(std::memory_order_relaxed);

    // 输出结果（兼容 Dashboard.exe 解析格式）
    printf("\n=== Results ===\n");
    printf("Total: %d msgs (order=%d exe=%d snap=%d)\n",
           result_.totalMsgs, result_.orderCnt, result_.exeCnt, result_.snapCnt);
    printf("Time:  %.3f s (%.0f msg/s)\n", result_.elapsedSec, result_.throughput);
    printf("Latency: p50=%.1fus p99=%.1fus p99.9=%.1fus pmax=%.1fus\n",
           result_.latency.p50 / 1000.0,
           result_.latency.p99 / 1000.0,
           result_.latency.p999 / 1000.0,
           result_.latency.pmax / 1000.0);

    printf("\nOrderBook State:\n%s\n", axob.toString().c_str());

    auto [askLevels, bidLevels] = axob.getLevels(5);
    printf("\n--- 5 Level OrderBook ---\n");
    for (int i = 4; i >= 0; i--) {
        auto it = askLevels.find(i);
        if (it != askLevels.end() && it->second.qty > 0)
            printf("  Ask[%d]  %d * %d\n", i, it->second.price, it->second.qty);
    }
    printf("  -----\n");
    for (int i = 0; i < 5; i++) {
        auto it = bidLevels.find(i);
        if (it != bidLevels.end() && it->second.qty > 0)
            printf("  Bid[%d]  %d * %d\n", i, it->second.price, it->second.qty);
    }
    fflush(stdout);
}

// =====================================================================
//  Pipeline::getResult() — 获取结果
// =====================================================================
Pipeline::Result Pipeline::getResult() const {
    return result_;
}
