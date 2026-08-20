// spsc_queue.h — 无锁单生产者单消费者环形队列 (LMAX Disruptor 简化版)
//
// 设计要点 (与 Disruptor 同源):
//   - 固定容量环形数组 (容量取 2 的幂, 下标用与运算)
//   - 写索引 (head_) 仅生产者写, 读索引 (tail_) 仅消费者写 — 两索引互不竞争
//   - acquire/release 内存序保证数据可见性, 无任何互斥锁
//   - push/pop 返回是否成功; 满/空由调用方决定等待策略 (回测场景: 忙等+yield)
//   - 通道语义: 1P1C, 禁止多生产者/多消费者共享同一队列
#pragma once
#include <atomic>
#include <memory>
#include <cstddef>
#include <cstring>
#include <type_traits>

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity) {
        size_t c = 1;
        while (c < capacity) c <<= 1;
        cap_ = c;
        mask_ = cap_ - 1;
        buf_.reset(new T[cap_]);
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // 生产者: 入队; 队列满返回 false (调用方等待后重试)
    bool push(const T& v) {
        const size_t h = head_.load(std::memory_order_relaxed);
        // acquire: 看到消费者推进 tail (release) — 该槽位数据已被取走, 可覆盖
        const size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= cap_) return false;
        buf_[h & mask_] = v;
        head_.store(h + 1, std::memory_order_release);   // 数据写完后发布
        return true;
    }
    bool push(T&& v) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= cap_) return false;
        buf_[h & mask_] = std::move(v);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // 消费者: 出队; 队列空返回 false
    bool pop(T& v) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        // acquire: 看到生产者发布 head (release) — 槽位数据已就绪
        const size_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;
        v = std::move(buf_[t & mask_]);
        tail_.store(t + 1, std::memory_order_release);    // 槽位释放, 生产者可覆盖
        return true;
    }

    // 消费者: 批量出队 (一次取空, 最大 count 条) — Disruptor 批处理理念,
    // 摊销原子操作与 cache miss。T 为 trivially-copyable 时 memcpy。
    size_t pop_batch(T* items, size_t count) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t n = (count < h - t) ? count : (h - t);
        if (n == 0) return 0;
        const size_t pos   = static_cast<size_t>(t & mask_);
        const size_t first = (pos + n <= cap_) ? n : (cap_ - pos);
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(items, &buf_[pos], first * sizeof(T));
            if (n > first) std::memcpy(items + first, &buf_[0], (n - first) * sizeof(T));
        } else {
            for (size_t i = 0; i < first; ++i) items[i] = std::move(buf_[pos + i]);
            for (size_t i = 0; i < n - first; ++i) items[first + i] = std::move(buf_[i]);
        }
        tail_.store(t + n, std::memory_order_release);    // 槽位释放, 生产者可覆盖
        return n;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    size_t debugHead() const { return head_.load(std::memory_order_relaxed); }  // TEMP DEBUG
    size_t debugTail() const { return tail_.load(std::memory_order_relaxed); }  // TEMP DEBUG

private:
    size_t cap_ = 0, mask_ = 0;
    std::unique_ptr<T[]> buf_;
    alignas(64) std::atomic<size_t> head_{0};   // 写索引 (仅生产者写)
    alignas(64) std::atomic<size_t> tail_{0};   // 读索引 (仅消费者写)
};
