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
//  MSVC 兼容的预取宏
// =====================================================================
#if defined(__GNUC__) || defined(__clang__)
    #define PREFETCH_READ(addr) __builtin_prefetch((addr), 0, 3)
#elif defined(_MSC_VER)
    #include <xmmintrin.h>
    #define PREFETCH_READ(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#else
    #define PREFETCH_READ(addr) ((void)0)
#endif

// =====================================================================
//  consumerThread — 消费者线程函数
//
//  优化策略：
//    1. 动态批次：一次取空队列（最大 MAX_BATCH），适应负载变化
//    2. 内存预取：处理当前消息时预取下一条到 L1 缓存
// =====================================================================
void consumerThread(axob::core::SPSCQueue<MarketEvent>& queue, AXOB& axob,
                    axob::core::LatencyStats& latency, ConsumerStats& stats) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // 一次取空，最大 1024 条（栈上分配，约 1024 * sizeof(MarketEvent) ≈ 128KB）
    static constexpr size_t MAX_BATCH = 1024;
    MarketEvent batch[MAX_BATCH];

    uint64_t totalConsumed = 0;
    uint64_t nextReport = 0;
    const uint64_t reportInterval = 234;  // 与 v1 一致
    int emptyCount = 0;

    // 主循环：一次取空队列，动态批次大小
    while (true) {
        // 一次取空，最大 MAX_BATCH 条
        size_t n = queue.pop_batch(batch, MAX_BATCH);

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
            // 预取下一条消息到 L1 缓存（只预取读取，最高时间局部性）
            if (i + 1 < n) {
                PREFETCH_READ(&batch[i + 1]);
            }

            // 计算延迟
            uint64_t dequeueTime = now_ns();
            uint64_t latencyNs = dequeueTime - batch[i].enqueueTimestamp;
            latency.record(latencyNs);

            // 根据类型分发到 AXOB
            switch (batch[i].type) {
                case EventType::ORDER:
                    axob.onMsg(batch[i].order);
                    stats.orderCnt++;
                    break;

                case EventType::EXEC:
                    axob.onMsg(batch[i].exec);
                    stats.exeCnt++;
                    break;

                case EventType::SNAP:
                    axob.onMsg(batch[i].snap);
                    stats.snapCnt++;
                    break;

                case EventType::SIGNAL:
                    axob.onMsg(batch[i].signal);
                    break;

                case EventType::END:
                    axob.ensureSnap();  // [v2.7] 确保最终快照状态正确
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
