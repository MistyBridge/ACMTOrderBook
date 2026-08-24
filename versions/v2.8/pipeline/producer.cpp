#include "producer.h"
#include "../tool/msg_util.h"
#include "../tool/mmap_file.h"  // [v2.3] mmap 文件读取器
#include <cstdio>
#include <chrono>
#include <atomic>

// [v2.3] mmap 模式开关
// 定义 USE_MMAP 启用 mmap 文件预加载，否则使用传统 ifstream
#ifndef USE_MMAP
#define USE_MMAP 1
#endif

// =====================================================================
//  跨平台 pause 指令封装
//
//  GCC/Clang: __asm__ __volatile__("pause" ::: "memory")
//    - "pause" 提示 CPU 当前处于 spin-wait 循环，降低功耗并避免
//      对共享缓存行的抢占式访问（memory-ordering hint）
//    - "memory" 编译器屏障，阻止跨此指令的内存操作重排
//
//  MSVC: _mm_pause()（来自 <intrin.h>）
//    - 等价于 x86 PAUSE 指令，但不接受 "memory" clobber
//    - 已在 CMakeLists.txt 中通过 /utf-8 选项确保头文件正确解析
// =====================================================================
#ifdef _MSC_VER
#include <intrin.h>
inline void cpu_pause() { _mm_pause(); }
#else
inline void cpu_pause() { __asm__ __volatile__("pause" ::: "memory"); }
#endif

// =====================================================================
//  producerThread — 生产者线程函数
//  replayCount: 重放次数，用于放大测试数据集
// =====================================================================
void producerThread(const char* dataFile, axob::core::SPSCQueue<MarketEvent>& queue, ProducerStats& stats, int replayCount) {
    auto t0 = std::chrono::high_resolution_clock::now();

    uint64_t totalProduced = 0;
    uint64_t nextReport = 0;
    const uint64_t reportInterval = 234;  // ~233875 / 1000，与 v1 一致

    // 重放循环
    for (int replay = 0; replay < replayCount; ++replay) {
        if (replayCount > 1) {
            printf("  [Replay %d/%d]\n", replay + 1, replayCount);
            fflush(stdout);
        }

        // [v2.3] 根据编译选项选择文件读取器
#if USE_MMAP
        // mmap 模式：预加载整个文件到内存，消除 I/O 瓶颈
        auto* reader = new MmapFileReader(std::string(dataFile));
        if (replay == 0) {
            printf("Using mmap file reader\n");
            fflush(stdout);
        }
#else
        // 传统模式：逐行读取
        // 用 new 分配 reader，规避 MinGW 8.1.0 -O2 std::ifstream 析构挂死
        // 故意不 delete，线程退出时 OS 回收内存
        auto* reader = new AxsbeFileReader(std::string(dataFile));
        if (replay == 0) {
            printf("Using ifstream file reader\n");
            fflush(stdout);
        }
#endif

        if (!reader->hasNext()) {
            printf("ERROR: file not found or empty\n");
            fflush(stdout);
            return;
        }

        if (replay == 0) {
            printf("File opened OK\n");
            fflush(stdout);
        }

        // 主循环：读取消息并推入队列
        while (reader->hasNext()) {
            AxsbeOrder order;
            AxsbeExe   exe;
            AxsbeSnapStock snap;
            int type = reader->next(order, exe, snap);

            // [v2.7] 零拷贝优化：直接在队列槽位上构造 MarketEvent
            MarketEvent* slot = nullptr;
            while (!(slot = queue.try_emplace_slot())) {
                for (int i = 0; i < 64; ++i) {
                    cpu_pause();
                }
                std::atomic_thread_fence(std::memory_order_acquire);
            }

            // 根据消息类型直接在槽位上构造
            if (isOrdType(type)) {
                *slot = MarketEvent::makeOrder(order);
            } else if (isExeType(type)) {
                *slot = MarketEvent::makeExe(exe);
            } else if (isSnapType(type)) {
                *slot = MarketEvent::makeSnap(snap);
            } else {
                // 跳过未知类型，但已占用槽位，需提交空事件
                slot->type = EventType::END;  // 标记为 END，消费者会忽略
            }

            // [v2.8] 延迟采样：只对 1/8 消息设置时间戳
            if ((totalProduced & 7) == 0) {
                slot->enqueueTimestamp = now_ns();
            }

            // 提交入队
            queue.commit_push();

            totalProduced++;

            // 进度报告
            if (totalProduced >= nextReport) {
                printf("  produced %llu msgs...\n", (unsigned long long)totalProduced);
                fflush(stdout);
                nextReport += reportInterval;
            }
        }

        // 显式关闭文件流，但不 delete reader（规避 MinGW -O2 析构挂死）
        reader->close();
    }

    // 发送 END 事件通知消费者退出
    MarketEvent* endSlot = nullptr;
    while (!(endSlot = queue.try_emplace_slot())) {
        for (int i = 0; i < 64; ++i) {
            cpu_pause();
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    }
    *endSlot = MarketEvent::makeEnd();
    queue.commit_push();

    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    // 更新统计
    stats.totalProduced.store(totalProduced, std::memory_order_relaxed);
    stats.totalTimeNs.store(elapsed, std::memory_order_relaxed);
}
