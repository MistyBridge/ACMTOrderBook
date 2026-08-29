#include <gtest/gtest.h>
#include "core/latency_stats.h"
#include <cstdint>

using axob::core::LatencyStats;

TEST(LatencyStats, EmptySnapshot) {
    LatencyStats stats(64);
    auto s = stats.snapshot();
    EXPECT_EQ(s.count, 0u);
    EXPECT_EQ(s.p50, 0u);
    EXPECT_EQ(s.pmax, 0u);
}

TEST(LatencyStats, PercentilesOfSamples) {
    // 容量取 128 > 100，避免环形缓冲绕回（否则只保留最后 capacity 个）
    LatencyStats stats(128);
    for (int i = 1; i <= 100; ++i) stats.record(static_cast<uint64_t>(i));
    auto s = stats.snapshot();
    EXPECT_EQ(s.count, 100u);
    // index = floor(p * (n-1)) with n=100 -> n-1=99
    EXPECT_EQ(s.p50, 50u);    // floor(0.5*99)=49 -> value 50
    EXPECT_EQ(s.p99, 99u);    // floor(0.99*99)=98 -> value 99
    EXPECT_EQ(s.p999, 99u);   // floor(0.999*99)=98 -> value 99
    EXPECT_EQ(s.pmax, 100u);  // max element
}

TEST(LatencyStats, RingBufferWrapAround) {
    LatencyStats stats(16);
    // write more than capacity -> wraps, snapshot should still give sane percentiles
    for (int i = 0; i < 100; ++i) stats.record(static_cast<uint64_t>(i));
    auto s = stats.snapshot();
    EXPECT_EQ(s.count, 16u);  // only last 16 kept
}

TEST(LatencyStats, ResetClears) {
    LatencyStats stats(64);
    for (int i = 0; i < 10; ++i) stats.record(static_cast<uint64_t>(i));
    EXPECT_EQ(stats.count(), 10u);
    stats.reset();
    EXPECT_EQ(stats.count(), 0u);
}
