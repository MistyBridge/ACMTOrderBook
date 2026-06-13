// =====================================================================
//  core/compile_test.cpp — 编译验证
//  验证 5 个 core 头文件能独立编译通过，并做基本功能冒烟测试。
//
//  编译命令（GCC / MinGW）:
//    g++ -std=c++17 -O2 -Wall -Wextra -I.. -o compile_test.exe compile_test.cpp
//
//  编译命令（MSVC）:
//    cl /std:c++17 /O2 /W4 /EHsc /I.. compile_test.cpp /Fe:compile_test.exe
// =====================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <atomic>
#include <functional>

#include "cache_line.h"
#include "spsc_queue.h"
#include "memory_pool.h"
#include "latency_stats.h"
#include "cpu_affinity.h"

using namespace axob::core;

// =====================================================================
//  多线程辅助：根据编译器/平台选择线程 API
// =====================================================================
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    // Windows 原生线程包装
    struct NativeThread {
        HANDLE h = nullptr;

        static DWORD WINAPI threadEntry(LPVOID param) {
            auto* fn = static_cast<std::function<void()>*>(param);
            (*fn)();
            delete fn;
            return 0;
        }

        template <typename Fn>
        explicit NativeThread(Fn&& func) {
            auto* fn = new std::function<void()>(std::forward<Fn>(func));
            h = CreateThread(nullptr, 0, threadEntry, fn, 0, nullptr);
        }
        void join() {
            if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); h = nullptr; }
        }
        ~NativeThread() { if (h) CloseHandle(h); }
        NativeThread(const NativeThread&) = delete;
        NativeThread& operator=(const NativeThread&) = delete;
    };

#else
    #include <pthread.h>
    #include <functional>

    struct NativeThread {
        pthread_t tid = 0;
        bool running = false;
        std::function<void()> func_;

        template <typename Fn>
        explicit NativeThread(Fn&& f) : func_(std::forward<Fn>(f)), running(true) {
            struct Arg { NativeThread* self; };
            auto* arg = new Arg{this};
            pthread_create(&tid, nullptr, [](void* p) -> void* {
                auto* a = static_cast<Arg*>(p);
                a->self->func_();
                delete a;
                return nullptr;
            }, arg);
        }
        void join() {
            if (running) { pthread_join(tid, nullptr); running = false; }
        }
        ~NativeThread() { if (running) pthread_detach(tid); }
        NativeThread(const NativeThread&) = delete;
        NativeThread& operator=(const NativeThread&) = delete;
    };
#endif

// ---- 测试对象 ----
struct TestEvent {
    uint64_t seq;
    uint64_t price;
    int32_t  qty;
    uint8_t  side;
    uint8_t  _pad[3];
};
static_assert(sizeof(TestEvent) >= sizeof(void*), "");

struct TestOrder {
    uint64_t orderId;
    uint64_t price;
    int32_t  qty;
    TestOrder* next;  // 使 sizeof >= sizeof(void*)
};

// ---- test_cache_line ----
void test_cache_line() {
    struct Shared {
        CacheLinePadded<std::atomic<uint64_t>> writeIdx;
        CacheLinePadded<std::atomic<uint64_t>> readIdx;
    };
    Shared s;
    s.writeIdx.value.store(42, std::memory_order_relaxed);
    s.readIdx.value.store(10, std::memory_order_relaxed);

    // 验证不在同一 cache line
    uintptr_t a = reinterpret_cast<uintptr_t>(&s.writeIdx);
    uintptr_t b = reinterpret_cast<uintptr_t>(&s.readIdx);
    assert(b - a >= CACHELINE_SIZE);

    // CACHELINE_PAD 测试：a(8B) + pad(56B) + b(8B) = 72B，b 跳到了下一条 cache line
    struct Padded {
        uint64_t a;
        CACHELINE_PAD(56);
        uint64_t b;
    };
    // 验证 b 的偏移 >= 64（在不同 cache line 上）
    assert(offsetof(Padded, b) >= CACHELINE_SIZE);

    // 普通类型测试
    CacheLinePadded<int> ci;
    ci.value = 42;
    assert(ci.value == 42);

    printf("  [PASS] cache_line.h\n");
}

// ---- test_spsc_queue ----
void test_spsc_queue() {
    // 基本 push/pop
    SPSCQueue<uint64_t> q(16);  // 实际容量 16
    assert(q.capacity() == 16);
    assert(q.empty());

    for (uint64_t i = 0; i < 16; ++i) {
        assert(q.try_push(i));
    }
    assert(!q.try_push(999ULL));  // 满
    assert(q.size() == 16);

    uint64_t val;
    for (uint64_t i = 0; i < 16; ++i) {
        assert(q.try_pop(val));
        assert(val == i);
    }
    assert(!q.try_pop(val));  // 空
    assert(q.empty());

    // 批量操作
    SPSCQueue<TestEvent> bq(64);
    TestEvent events[32];
    for (int i = 0; i < 32; ++i) {
        events[i] = {uint64_t(i), 1000, 100, 1, {}};
    }
    size_t pushed = bq.push_batch(events, 32);
    assert(pushed == 32);

    TestEvent out[64];
    size_t popped = bq.pop_batch(out, 64);
    assert(popped == 32);
    for (int i = 0; i < 32; ++i) {
        assert(out[i].seq == uint64_t(i));
    }

    // 多线程生产-消费（使用原生线程，兼容所有编译器）
    SPSCQueue<uint64_t> mq(1024);
    constexpr int TOTAL = 100000;

    NativeThread producer([&]() {
        for (int i = 0; i < TOTAL; ++i) {
            while (!mq.try_push(uint64_t(i))) {
                // busy wait
            }
        }
    });

    NativeThread consumer([&]() {
        int received = 0;
        uint64_t prev = 0;
        while (received < TOTAL) {
            uint64_t v;
            if (mq.try_pop(v)) {
                if (received > 0) {
                    assert(v >= prev);  // SPSC 保序
                }
                prev = v;
                ++received;
            }
        }
    });

    producer.join();
    consumer.join();
    assert(mq.empty());

    printf("  [PASS] spsc_queue.h\n");
}

// ---- test_memory_pool ----
void test_memory_pool() {
    MemoryPool<TestOrder> pool(64);
    assert(pool.totalSlots() >= 64);
    assert(pool.freeSlots() == pool.totalSlots());

    // 分配所有 slot
    TestOrder* orders[64];
    for (int i = 0; i < 64; ++i) {
        orders[i] = pool.alloc();
        orders[i]->orderId = uint64_t(i);
        orders[i]->price = 1000;
    }
    assert(pool.freeSlots() == 0);

    // 归还一半
    for (int i = 0; i < 32; ++i) {
        pool.free(orders[i]);
    }
    assert(pool.freeSlots() == 32);

    // 再分配（复用归还的 slot）
    for (int i = 0; i < 32; ++i) {
        TestOrder* o = pool.alloc();
        assert(o != nullptr);
    }

    // 强制扩容
    TestOrder* extra = pool.alloc();
    assert(extra != nullptr);
    assert(pool.totalSlots() > 64);

    printf("  [PASS] memory_pool.h\n");
}

// ---- test_latency_stats ----
void test_latency_stats() {
    LatencyStats stats(1024);
    assert(stats.capacity() >= 1024);
    assert(stats.count() == 0);

    // 录入已知分布: 0, 100, 200, ..., 9900
    for (uint64_t i = 0; i < 100; ++i) {
        stats.record(i * 100);
    }
    assert(stats.count() == 100);

    auto s = stats.snapshot();
    assert(s.count == 100);
    // p50 = 5000 (第 50 个元素, 0-indexed)
    assert(s.p50 >= 4800 && s.p50 <= 5200);
    // pmax = 9900
    assert(s.pmax == 9900);

    // 微秒便捷方法
    double us = stats.p50_us();
    assert(us >= 4.8 && us <= 5.2);

    // reset
    stats.reset();
    assert(stats.count() == 0);
    auto s2 = stats.snapshot();
    assert(s2.count == 0);

    printf("  [PASS] latency_stats.h\n");
}

// ---- test_cpu_affinity ----
void test_cpu_affinity() {
    int cores = getPhysicalCoreCount();
    assert(cores >= 1);
    printf("  [INFO] Physical cores detected: %d\n", cores);

#if defined(_WIN32)
    // Windows: 测试原生句柄版本
    HANDLE hSelf = GetCurrentThread();
    // 绑定到核心 0（不保证成功，只验证不崩溃）
    bool ok = setThreadAffinityByHandle(hSelf, 0);
    printf("  [INFO] setThreadAffinityByHandle(current, 0) = %s\n", ok ? "true" : "false");

#if defined(AXOB_HAS_STD_THREAD)
    // 如果 std::thread 可用，也测试一下
    // 注意：这里不创建新线程，只验证编译通过
    printf("  [INFO] std::thread version available\n");
#endif

#elif defined(__linux__) || defined(__APPLE__)
    // POSIX: 测试原生线程 ID 版本
    pthread_t self = pthread_self();
    bool ok = setThreadAffinityByNative(self, 0);
    printf("  [INFO] setThreadAffinityByNative(self, 0) = %s\n", ok ? "true" : "false");
#endif

    printf("  [PASS] cpu_affinity.h\n");
}

// ---- main ----
int main() {
    printf("=== AXOrderBook v2 core module compile & smoke test ===\n\n");

    test_cache_line();
    test_spsc_queue();
    test_memory_pool();
    test_latency_stats();
    test_cpu_affinity();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}