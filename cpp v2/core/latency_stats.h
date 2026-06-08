#pragma once
// =====================================================================
//  core/latency_stats.h — 延迟统计器
//
//  使用固定大小环形缓冲区存储采样值（默认 65536 条），
//  record() 为 O(1) 的简单写入。
//  snapshot() 内部拷贝有效数据后使用 std::nth_element (O(n)) 计算
//  p50 / p99 / p999 / pmax 百分位，不修改原始数据。
//
//  用法示例：
//    LatencyStats stats;
//    stats.record(1500);   // 1500 ns = 1.5 μs
//    stats.record(2300);
//    auto s = stats.snapshot();
//    printf("p50=%.1fμs p99=%.1fμs pmax=%.1fμs\n",
//           s.p50 / 1000.0, s.p99 / 1000.0, s.pmax / 1000.0);
// =====================================================================

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "cache_line.h"

namespace axob {
namespace core {

class LatencyStats {
public:
    // 统计快照结构体
    struct Stats {
        uint64_t p50;     // 中位数（纳秒）
        uint64_t p90;     // 90 百分位（纳秒）
        uint64_t p99;     // 99 百分位（纳秒）
        uint64_t p999;    // 99.9 百分位（纳秒）
        uint64_t pmax;    // 最大值（纳秒）
        size_t   count;   // 有效采样数
    };

    // ---------------------------------------------------------------
    //  构造函数
    //  bufferSize: 环形缓冲区容量，向上取整到 2 的幂（默认 65536）
    // ---------------------------------------------------------------
    explicit LatencyStats(size_t bufferSize = 65536)
        : capacity_(roundUpPow2(bufferSize))
        , mask_(capacity_ - 1)
    {
        // 分配 cache line 对齐的缓冲区
        size_t bytes = capacity_ * sizeof(uint64_t);
        const size_t alignment = CACHELINE_SIZE;
        bytes = (bytes + alignment - 1) & ~(alignment - 1);

#ifdef _WIN32
        buffer_ = static_cast<uint64_t*>(_aligned_malloc(bytes, alignment));
        if (!buffer_) std::abort();
#else
        buffer_ = static_cast<uint64_t*>(std::aligned_alloc(alignment, bytes));
        if (!buffer_) std::abort();
#endif
    }

    ~LatencyStats() {
#ifdef _WIN32
        _aligned_free(buffer_);
#else
        std::free(buffer_);
#endif
    }

    // 禁止拷贝/移动
    LatencyStats(const LatencyStats&)            = delete;
    LatencyStats& operator=(const LatencyStats&) = delete;
    LatencyStats(LatencyStats&&)                 = delete;
    LatencyStats& operator=(LatencyStats&&)      = delete;

    // ---------------------------------------------------------------
    //  record(latencyNs) —— 记录一条延迟采样（纳秒），O(1)
    // ---------------------------------------------------------------
    void record(uint64_t latencyNs) noexcept {
        uint64_t idx = writeIdx_.fetch_add(1, std::memory_order_relaxed);
        buffer_[idx & mask_] = latencyNs;
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------
    //  reset() —— 清空所有采样
    // ---------------------------------------------------------------
    void reset() noexcept {
        writeIdx_.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------
    //  snapshot() —— 返回当前统计快照（const，不修改原始数据）
    //  内部使用 std::nth_element 实现 O(n) 百分位计算
    // ---------------------------------------------------------------
    Stats snapshot() const {
        const uint64_t total = count_.load(std::memory_order_relaxed);
        const size_t n = std::min(static_cast<size_t>(total), capacity_);
        if (n == 0) return Stats{0, 0, 0, 0, 0, 0};

        // 拷贝有效数据到临时数组
        const uint64_t w = writeIdx_.load(std::memory_order_relaxed);
        std::vector<uint64_t> tmp(n);

        if (total <= capacity_) {
            // 尚未绕回：数据在 [0, n)
            for (size_t i = 0; i < n; ++i) {
                tmp[i] = buffer_[i];
            }
        } else {
            // 已绕回：数据在 [writeIdx_ % capacity_, writeIdx_ % capacity_ + capacity_)
            const size_t start = static_cast<size_t>(w & mask_);
            for (size_t i = 0; i < n; ++i) {
                tmp[i] = buffer_[(start + i) & mask_];
            }
        }

        // 使用 nth_element 逐级定位百分位
        // 注意：后续 nth_element 可能移动前一级的迭代器所指元素，
        //       因此每次 nth_element 之后立即保存结果。
        auto p50_it  = tmp.begin() + static_cast<ptrdiff_t>(n / 2);
        auto p90_it  = tmp.begin() + static_cast<ptrdiff_t>(n * 90 / 100);
        auto p99_it  = tmp.begin() + static_cast<ptrdiff_t>(n * 99 / 100);
        auto p999_it = tmp.begin() + static_cast<ptrdiff_t>(n * 999 / 1000);

        // 第 1 步：定位 p50
        if (p50_it != tmp.begin()) {
            std::nth_element(tmp.begin(), p50_it, tmp.end());
        }
        uint64_t p50 = *p50_it;

        // 第 2 步：在 [p50, end) 中定位 p90
        uint64_t p90 = p50;  // 若 p90 == p50 则无需再次 partition
        if (p90_it != p50_it) {
            std::nth_element(p50_it, p90_it, tmp.end());
            p90 = *p90_it;
        }

        // 第 3 步：在 [p90, end) 中定位 p99
        uint64_t p99 = p90;  // 若 p99 == p90 则无需再次 partition
        if (p99_it != p90_it) {
            std::nth_element(p90_it, p99_it, tmp.end());
            p99 = *p99_it;
        }

        // 第 4 步：在 [p99, end) 中定位 p999
        uint64_t p999 = p99;
        if (p999_it != p99_it) {
            std::nth_element(p99_it, p999_it, tmp.end());
            p999 = *p999_it;
        }

        // pmax: [p999_it, end) 中的最大值
        uint64_t pmax = *std::max_element(p999_it, tmp.end());

        return Stats{p50, p90, p99, p999, pmax, n};
    }

    // ---------------------------------------------------------------
    //  便捷方法：以微秒 (double) 返回百分位
    // ---------------------------------------------------------------
    double p50_us()  const { return static_cast<double>(snapshot().p50)  / 1000.0; }
    double p99_us()  const { return static_cast<double>(snapshot().p99)  / 1000.0; }
    double p999_us() const { return static_cast<double>(snapshot().p999) / 1000.0; }
    double pmax_us() const { return static_cast<double>(snapshot().pmax) / 1000.0; }

    // ---------------------------------------------------------------
    //  当前已记录的采样总数（近似值）
    // ---------------------------------------------------------------
    size_t count() const noexcept {
        return static_cast<size_t>(
            count_.load(std::memory_order_relaxed));
    }

    // ---------------------------------------------------------------
    //  环形缓冲区容量
    // ---------------------------------------------------------------
    size_t capacity() const noexcept { return capacity_; }

private:
    static size_t roundUpPow2(size_t v) noexcept {
        if (v <= 1) return 2;
        v--;
        v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
        v |= v >> 8;  v |= v >> 16;
        if constexpr (sizeof(size_t) > 4) {
            v |= v >> 32;
        }
        return v + 1;
    }

    const size_t capacity_;  // 环形缓冲区容量（2 的幂）
    const size_t mask_;      // capacity_ - 1

    alignas(CACHELINE_SIZE) uint64_t* buffer_;  // 采样缓冲区

    alignas(CACHELINE_SIZE) std::atomic<uint64_t> writeIdx_{0};  // 写指针
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> count_{0};     // 总采样计数
};

}  // namespace core
}  // namespace axob