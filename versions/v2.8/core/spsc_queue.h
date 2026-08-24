#pragma once
// =====================================================================
//  core/spsc_queue.h — 单生产者单消费者无锁队列
//
//  环形缓冲区实现，容量强制为 2 的幂（位掩模替代取模）。
//  生产者只写 writeIndex_，消费者只写 readIndex_，
//  两个 index 各占独立 cache line，彻底消除伪共享。
//  不使用 std::mutex，纯原子操作。
//
//  内存序：
//    push  — store 用 memory_order_release
//    pop   — load  用 memory_order_acquire
//    size  — load  均用 memory_order_relaxed
//
//  模板参数 T 应为 trivially-copyable 类型以获得最佳性能。
//
//  用法示例：
//    SPSCQueue<MarketEvent> queue(1024);       // 实际容量 1024 (2^10)
//    queue.push_batch(events, 64);
//    size_t n = queue.pop_batch(buffer, 64);
// =====================================================================

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include "cache_line.h"
#include "huge_pages.h"

namespace axob {
namespace core {

template <typename T>
class SPSCQueue {
public:
    // capacityHint: 期望容量，构造时向上取整到最近的 2 的幂
    explicit SPSCQueue(size_t capacityHint)
        : capacity_(roundUpPow2(capacityHint))
        , mask_(capacity_ - 1)
    {
        assert(capacityHint > 0 && capacity_ >= 2);

        // [v2.5] 分配缓冲区：优先使用大页（2MB），回退到 cache line 对齐
        size_t bytes = capacity_ * sizeof(T);
        const size_t alignment = CACHELINE_SIZE;
        bytes = (bytes + alignment - 1) & ~(alignment - 1);

        // 尝试大页分配（减少 TLB miss）
        bufferBytes_ = bytes;
        buffer_ = static_cast<T*>(allocLargePages(bytes));
        if (buffer_) {
            usedLargePages_ = true;
        } else {
            // 回退到对齐分配
            usedLargePages_ = false;
#ifdef _WIN32
            buffer_ = static_cast<T*>(_aligned_malloc(bytes, alignment));
#else
            buffer_ = static_cast<T*>(std::aligned_alloc(alignment, bytes));
#endif
            if (!buffer_) std::abort();
        }
    }

    ~SPSCQueue() {
        // 析构缓冲区中残留的元素
        uint64_t r = readIndexVal_.load(std::memory_order_relaxed);
        uint64_t w = writeIndexVal_.load(std::memory_order_relaxed);
        for (uint64_t i = r; i < w; ++i) {
            buffer_[i & mask_].~T();
        }
        // [v2.5] 根据分配方式选择释放方法
        if (usedLargePages_) {
            freeLargePages(buffer_, bufferBytes_);
        } else {
#ifdef _WIN32
            _aligned_free(buffer_);
#else
            std::free(buffer_);
#endif
        }
    }

    // 禁止拷贝/移动
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&)                 = delete;
    SPSCQueue& operator=(SPSCQueue&&)      = delete;

    // ---------------------------------------------------------------
    //  [v2.7] 零拷贝入队：返回槽位指针，生产者直接构造
    //  调用者获得一个 T* 指针，可 placement new 或直接赋值
    //  必须随后调用 commit_push() 提交
    // ---------------------------------------------------------------
    T* try_emplace_slot() {
        const uint64_t w = writeIndexVal_.load(std::memory_order_relaxed);
        const uint64_t r = readIndexVal_.load(std::memory_order_acquire);
        if (w - r >= capacity_) return nullptr;  // 满
        return &buffer_[w & mask_];
    }

    void commit_push() {
        writeIndexVal_.store(
            writeIndexVal_.load(std::memory_order_relaxed) + 1,
            std::memory_order_release);
    }

    // ---------------------------------------------------------------
    //  单条出队
    //  队列空时返回 false
    // ---------------------------------------------------------------
    bool try_pop(T& item) {
        const uint64_t r = readIndexVal_.load(std::memory_order_relaxed);
        const uint64_t w = writeIndexVal_.load(std::memory_order_acquire);
        if (r >= w) return false;  // 空

        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(&item, &buffer_[r & mask_], sizeof(T));
        } else {
            item = static_cast<T&&>(buffer_[r & mask_]);
            buffer_[r & mask_].~T();
        }
        readIndexVal_.store(r + 1, std::memory_order_release);
        return true;
    }

    // ---------------------------------------------------------------
    //  批量入队
    //  将 items[0..count-1] 依次入队，返回实际入队数量
    //  队列剩余空间不足时只入队部分元素
    // ---------------------------------------------------------------
    size_t push_batch(const T* items, size_t count) {
        const uint64_t w = writeIndexVal_.load(std::memory_order_relaxed);
        const uint64_t r = readIndexVal_.load(std::memory_order_acquire);
        const size_t avail = capacity_ - static_cast<size_t>(w - r);
        const size_t n = (count < avail) ? count : avail;
        if (n == 0) return 0;

        const size_t pos   = static_cast<size_t>(w & mask_);
        const size_t first = (pos + n <= capacity_) ? n : (capacity_ - pos);

        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(&buffer_[pos], items, first * sizeof(T));
            if (n > first) {
                std::memcpy(&buffer_[0], items + first, (n - first) * sizeof(T));
            }
        } else {
            for (size_t i = 0; i < first; ++i) {
                new (&buffer_[pos + i]) T(items[i]);
            }
            for (size_t i = 0; i < n - first; ++i) {
                new (&buffer_[i]) T(items[first + i]);
            }
        }
        writeIndexVal_.store(w + n, std::memory_order_release);
        return n;
    }

    // ---------------------------------------------------------------
    //  批量出队
    //  将至多 count 个元素出队到 items[]，返回实际出队数量
    // ---------------------------------------------------------------
    size_t pop_batch(T* items, size_t count) {
        const uint64_t r = readIndexVal_.load(std::memory_order_relaxed);
        const uint64_t w = writeIndexVal_.load(std::memory_order_acquire);
        const size_t avail = static_cast<size_t>(w - r);
        const size_t n = (count < avail) ? count : avail;
        if (n == 0) return 0;

        const size_t pos   = static_cast<size_t>(r & mask_);
        const size_t first = (pos + n <= capacity_) ? n : (capacity_ - pos);

        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(items, &buffer_[pos], first * sizeof(T));
            if (n > first) {
                std::memcpy(items + first, &buffer_[0], (n - first) * sizeof(T));
            }
        } else {
            for (size_t i = 0; i < first; ++i) {
                items[i] = static_cast<T&&>(buffer_[pos + i]);
                buffer_[pos + i].~T();
            }
            for (size_t i = 0; i < n - first; ++i) {
                items[first + i] = static_cast<T&&>(buffer_[i]);
                buffer_[i].~T();
            }
        }
        readIndexVal_.store(r + n, std::memory_order_release);
        return n;
    }

    // ---------------------------------------------------------------
    //  当前元素数量（近似值，可能略大于实际值）
    // ---------------------------------------------------------------
    size_t size() const noexcept {
        const uint64_t w = writeIndexVal_.load(std::memory_order_relaxed);
        const uint64_t r = readIndexVal_.load(std::memory_order_relaxed);
        return static_cast<size_t>(w - r);
    }

    // ---------------------------------------------------------------
    //  队列是否为空（近似判断）
    // ---------------------------------------------------------------
    bool empty() const noexcept {
        return size() == 0;
    }

    // ---------------------------------------------------------------
    //  队列容量（构造时指定的 2 的幂）
    // ---------------------------------------------------------------
    size_t capacity() const noexcept { return capacity_; }

private:
    // 将 v 向上取整到最近的 2 的幂（v 本身为 2 的幂时不变）
    static size_t roundUpPow2(size_t v) noexcept {
        // 处理 v == 0 和 v == 1 的边界
        if (v <= 1) return 2;
        v--;
        v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
        v |= v >> 8;  v |= v >> 16;
        if constexpr (sizeof(size_t) > 4) {
            v |= v >> 32;
        }
        return v + 1;
    }

    // ---- 数据成员 ----
    const size_t capacity_;  // 缓冲区容量（2 的幂）
    const size_t mask_;      // capacity_ - 1，用于位掩模

    T* buffer_;              // 环形缓冲区
    size_t bufferBytes_ = 0;     // [v2.5] 缓冲区字节数（用于大页释放）
    bool usedLargePages_ = false; // [v2.5] 是否使用了大页分配

    // 写索引 —— 仅生产者线程写，独占 cache line
    // 直接使用 alignas 避免 CacheLinePadded 在 -O2 下的别名分析问题
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> writeIndexVal_{0};

    // 读索引 —— 仅消费者线程写，独占 cache line
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> readIndexVal_{0};
};

}  // namespace core
}  // namespace axob