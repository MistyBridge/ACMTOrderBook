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
    printf("\n--- 5 Level OrderBook (internal ×10^5) ---\n");
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
