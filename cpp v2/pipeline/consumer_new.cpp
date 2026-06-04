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
//  MSVC compatible prefetch macro
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
//  consumerThread
//
//  [v2.8] Latency sampling on every 8th messages (producer sets timestamp)
//  Saves ~14ns/msg (7/8 messages skip now_ns + latency.record)
// =====================================================================
void consumerThread(axob::core::SPSCQueue<MarketEvent>& queue, AXOB& axob,
                    axob::core::LatencyStats& latency, ConsumerStats& stats) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // batch: 64 * sizeof(MarketEvent) ~ 8KB, cache-friendly
    static constexpr size_t MAX_BATCH = 64;
    MarketEvent batch[MAX_BATCH];

    uint64_t totalConsumed = 0;
    uint64_t nextReport = 0;
    const uint64_t reportInterval = 234;
    int emptyCount = 0;

    // Main loop: drain queue in batches
    while (true) {
        size_t n = queue.pop_batch(batch, MAX_BATCH);

        if (n == 0) {
            emptyCount++;
            if (emptyCount < 10) {
                threadYield();
            } else {
                threadYield();
                emptyCount = 0;
            }
            continue;
        }

        emptyCount = 0;

        // Process batch
        for (size_t i = 0; i < n; ++i) {
            // Prefetch next message into L1
            if (i + 1 < n) {
                PREFETCH_READ(&batch[i + 1]);
            }

            // Dispatch to AXOB by type
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
                    axob.ensureSnap();
                    goto done;
            }

            totalConsumed++;

            // Progress report
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

    stats.totalConsumed.store(totalConsumed, std::memory_order_relaxed);
    stats.totalTimeNs.store(elapsed, std::memory_order_relaxed);
}

