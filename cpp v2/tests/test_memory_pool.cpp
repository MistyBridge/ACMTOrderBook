#include <gtest/gtest.h>
#include "core/memory_pool.h"
#include <cstdint>
#include <vector>

using axob::core::MemoryPool;

// T must satisfy sizeof(T) >= sizeof(void*) (intrusive free list)
struct Ob { void* pad; int64_t a; int64_t b; };

TEST(MemoryPool, InitialAllocation) {
    MemoryPool<Ob> pool(4);
    EXPECT_GE(pool.totalSlots(), 4u);
    EXPECT_EQ(pool.freeSlots(), pool.totalSlots());
}

TEST(MemoryPool, AllocFreeRoundtrip) {
    MemoryPool<Ob> pool(4);
    Ob* p = pool.alloc();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.freeSlots(), pool.totalSlots() - 1);
    pool.free(p);
    EXPECT_EQ(pool.freeSlots(), pool.totalSlots());
}

TEST(MemoryPool, DistinctPointers) {
    MemoryPool<Ob> pool(4);
    Ob* a = pool.alloc();
    Ob* b = pool.alloc();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
}

TEST(MemoryPool, GrowsWhenExhausted) {
    MemoryPool<Ob> pool(2);
    std::vector<Ob*> ptrs;
    for (int i = 0; i < 10; ++i) ptrs.push_back(pool.alloc());
    bool distinct = true;
    for (size_t i = 0; i < ptrs.size(); ++i)
        for (size_t j = i + 1; j < ptrs.size(); ++j)
            if (ptrs[i] == ptrs[j]) distinct = false;
    EXPECT_TRUE(distinct);
    EXPECT_GE(pool.totalSlots(), 10u);
    // free all
    for (auto* p : ptrs) pool.free(p);
    EXPECT_EQ(pool.freeSlots(), pool.totalSlots());
}
