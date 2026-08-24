#include "producer.h"
#include "../tool/msg_util.h"
#include <cstdio>
#include <chrono>
#include <atomic>

// =====================================================================
//  producerThread — 生产者线程函数
// =====================================================================
void producerThread(const char* dataFile, axob::core::SPSCQueue<MarketEvent>& queue, ProducerStats& stats) {
    auto t0 = std::chrono::high_resolution_clock::now();

    uint64_t totalProduced = 0;
    uint64_t nextReport = 0;
    const uint64_t reportInterval = 234;  // ~233875 / 1000，与 v1 一致

    // 用 new 分配 reader，规避 MinGW 8.1.0 -O2 std::ifstream 析构挂死
    // 故意不 delete，线程退出时 OS 回收内存
    auto* reader = new AxsbeFileReader(std::string(dataFile));

    if (!reader->hasNext()) {
        printf("ERROR: file not found or empty\n");
        fflush(stdout);
        return;
    }
    printf("File opened OK\n");
    fflush(stdout);

    // 主循环：读取消息并推入队列
    while (reader->hasNext()) {
        AxsbeOrder order;
        AxsbeExe   exe;
        AxsbeSnapStock snap;
        int type = reader->next(order, exe, snap);

        // 根据消息类型构建 MarketEvent
        MarketEvent ev;
        if (isOrdType(type)) {
            ev = MarketEvent::makeOrder(order);
        } else if (isExeType(type)) {
            ev = MarketEvent::makeExe(exe);
        } else if (isSnapType(type)) {
            ev = MarketEvent::makeSnap(snap);
        } else {
            continue;  // 跳过未知类型
        }

        // 尝试推入队列，队列满时 spin 等待
        while (!queue.try_push(ev)) {
            for (int i = 0; i < 64; ++i) {
                __asm__ __volatile__("pause" ::: "memory");
            }
            std::atomic_thread_fence(std::memory_order_acquire);
        }

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

    // 发送 END 事件通知消费者退出
    MarketEvent endEv = MarketEvent::makeEnd();
    while (!queue.try_push(endEv)) {
        for (int i = 0; i < 64; ++i) {
            __asm__ __volatile__("pause" ::: "memory");
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    // 更新统计
    stats.totalProduced.store(totalProduced, std::memory_order_relaxed);
    stats.totalTimeNs.store(elapsed, std::memory_order_relaxed);
}
