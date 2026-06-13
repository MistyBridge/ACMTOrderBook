#include "consumer.h"
#include <cstdio>
#include <chrono>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    inline void threadYield() { SwitchToThread(); }
#else
    #include <sched.h>
    inline void threadYield() { sched_yield(); }
#endif

// =====================================================================
//  consumerThread — 消费者线程函数
// =====================================================================
void consumerThread(axob::core::SPSCQueue<MarketEvent>& queue, AXOB& axob,
                    axob::core::LatencyStats& latency, ConsumerStats& stats) {
    auto t0 = std::chrono::high_resolution_clock::now();

    MarketEvent batch[64];  // 批量缓冲区
    uint64_t totalConsumed = 0;
    uint64_t nextReport = 0;
    const uint64_t reportInterval = 234;  // 与 v1 一致
    int emptyCount = 0;

    // 主循环：批量取出消息并处理
    while (true) {
        size_t n = queue.pop_batch(batch, 64);

        if (n == 0) {
            // 队列空，等待
            emptyCount++;
            if (emptyCount < 10) {
                threadYield();
            } else {
                threadYield();
                emptyCount = 0;
            }
            continue;
        }

        emptyCount = 0;  // 重置空计数

        // 遍历 batch，根据 type 分发
        for (size_t i = 0; i < n; ++i) {
            const MarketEvent& ev = batch[i];

            // 计算延迟
            uint64_t dequeueTime = now_ns();
            uint64_t latencyNs = dequeueTime - ev.enqueueTimestamp;
            latency.record(latencyNs);

            // 根据类型分发到 AXOB
            switch (ev.type) {
                case EventType::ORDER:
                    axob.onMsg(ev.order);
                    stats.orderCnt++;
                    break;

                case EventType::EXEC:
                    axob.onMsg(ev.exec);
                    stats.exeCnt++;
                    break;

                case EventType::SNAP:
                    axob.onMsg(ev.snap);
                    stats.snapCnt++;
                    break;

                case EventType::SIGNAL:
                    axob.onMsg(ev.signal);
                    break;

                case EventType::END:
                    goto done;
            }

            totalConsumed++;

            // 进度报告
            if (totalConsumed >= nextReport) {
                printf("  consumed %llu msgs...\n", (unsigned long long)totalConsumed);
                fflush(stdout);
                nextReport += reportInterval;
            }
        }
    }

done:
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    // 更新统计
    stats.totalConsumed.store(totalConsumed, std::memory_order_relaxed);
    stats.totalTimeNs.store(elapsed, std::memory_order_relaxed);
}
