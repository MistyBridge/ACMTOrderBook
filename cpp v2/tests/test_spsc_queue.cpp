#include <gtest/gtest.h>
#include "core/spsc_queue.h"
#include <cstdint>
#include <array>

using axob::core::SPSCQueue;

TEST(SPSCQueue, CapacityRoundedUpToPow2) {
    SPSCQueue<uint64_t> q(10);
    EXPECT_EQ(q.capacity(), 16);
    EXPECT_TRUE(q.empty());

    SPSCQueue<uint64_t> q1(1);
    EXPECT_EQ(q1.capacity(), 2);

    SPSCQueue<uint64_t> q2(16);
    EXPECT_EQ(q2.capacity(), 16);
}

TEST(SPSCQueue, PushPopRoundtrip) {
    SPSCQueue<uint64_t> q(8);
    std::array<uint64_t, 5> in{{1, 2, 3, 4, 5}};
    EXPECT_EQ(q.push_batch(in.data(), in.size()), 5u);
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 5u);

    std::array<uint64_t, 8> out{};
    EXPECT_EQ(q.pop_batch(out.data(), out.size()), 5u);
    for (size_t i = 0; i < 5; ++i) EXPECT_EQ(out[i], in[i]);
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueue, PopEmptyReturnsZero) {
    SPSCQueue<uint64_t> q(8);
    std::array<uint64_t, 8> out{};
    EXPECT_EQ(q.pop_batch(out.data(), out.size()), 0u);

    uint64_t v = 0;
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SPSCQueue, FullPushOnlyWhatFits) {
    // capacity rounded to 4
    SPSCQueue<uint64_t> q(4);
    std::array<uint64_t, 10> in;
    for (size_t i = 0; i < 10; ++i) in[i] = i;

    // avail = capacity - (w - r) = 4
    EXPECT_EQ(q.push_batch(in.data(), 3), 3u);
    EXPECT_EQ(q.push_batch(in.data() + 3, 3), 1u);  // only 1 slot left
    // now full (w - r = 4), so nothing more fits
    EXPECT_EQ(q.push_batch(in.data() + 4, 2), 0u);

    std::array<uint64_t, 8> out{};
    EXPECT_EQ(q.pop_batch(out.data(), out.size()), 4u);
    EXPECT_EQ(out[0], 0u);
    EXPECT_EQ(out[1], 1u);
    EXPECT_EQ(out[2], 2u);
    EXPECT_EQ(out[3], 3u);
}

TEST(SPSCQueue, WrapAround) {
    SPSCQueue<uint64_t> q(4);
    std::array<uint64_t, 3> a{{10, 20, 30}};
    std::array<uint64_t, 3> out{};
    for (int round = 0; round < 5; ++round) {
        q.push_batch(a.data(), 3);
        EXPECT_EQ(q.pop_batch(out.data(), 3), 3u);
        for (size_t i = 0; i < 3; ++i) EXPECT_EQ(out[i], a[i]);
    }
}

TEST(SPSCQueue, ZeroCopyEmplaceAndCommit) {
    SPSCQueue<uint64_t> q(4);
    auto* slot = q.try_emplace_slot();
    ASSERT_NE(slot, nullptr);
    *slot = 123u;
    q.commit_push();
    EXPECT_EQ(q.size(), 1u);

    uint64_t v = 0;
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 123u);
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueue, EmplaceBlockedWhenFull) {
    SPSCQueue<uint64_t> q(2);
    auto a = q.try_emplace_slot(); ASSERT_NE(a, nullptr); *a = 1; q.commit_push();
    auto b = q.try_emplace_slot(); ASSERT_NE(b, nullptr); *b = 2; q.commit_push();
    // capacity 2 now full
    EXPECT_EQ(q.try_emplace_slot(), nullptr);
}
