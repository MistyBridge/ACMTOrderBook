#include <gtest/gtest.h>
#include "behave/ob_types.h"
#include <cstdint>

TEST(HybridLevelBook, InsertFindErase) {
    HybridLevelBook book;
    book.insert(1000, 5);
    book.insert(999, 3);
    book.insert(1001, 7);

    EXPECT_EQ(book.size(), 3);
    const LevelNode* n = book.find(1000);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->qty, 5);

    // array mode keeps ascending order for forward (ask) iteration
    EXPECT_EQ(book.levels[0].price, 999);
    EXPECT_EQ(book.levels[1].price, 1000);
    EXPECT_EQ(book.levels[2].price, 1001);

    book.erase(1000);
    EXPECT_EQ(book.size(), 2);
    EXPECT_EQ(book.find(1000), nullptr);
}

TEST(HybridLevelBook, BestBidBestAsk) {
    HybridLevelBook book;
    book.insert(1000, 1);
    book.insert(1002, 3);
    book.insert(1001, 2);
    EXPECT_EQ(book.bestAsk()->price, 1000);  // lowest
    EXPECT_EQ(book.bestBid()->price, 1002);  // highest
}

TEST(HybridLevelBook, ModifyQty) {
    HybridLevelBook book;
    book.insert(1000, 5);
    EXPECT_TRUE(book.modifyQty(1000, 3));
    EXPECT_EQ(book.find(1000)->qty, 8);
    EXPECT_FALSE(book.modifyQty(9999, 1));
}

TEST(HybridLevelBook, ForEachOrder) {
    HybridLevelBook book;
    book.insert(1000, 1);
    book.insert(1002, 3);
    book.insert(1001, 2);
    // forward ascending
    int64_t prev = -1;
    int cnt = 0;
    book.for_each([&](const LevelNode& l) { EXPECT_GT(l.price, prev); prev = l.price; ++cnt; });
    EXPECT_EQ(cnt, 3);
    // reverse descending
    prev = INT64_MAX; cnt = 0;
    book.rfor_each([&](const LevelNode& l) { EXPECT_LT(l.price, prev); prev = l.price; ++cnt; });
    EXPECT_EQ(cnt, 3);
}

TEST(HybridLevelBook, MigrateToMapBeyondThreshold) {
    HybridLevelBook book;
    // insert beyond HYBRID_ARRAY_MAX (256) -> switches to std::map
    for (int i = 0; i < 260; ++i) book.insert(1000 + i, 1);
    EXPECT_TRUE(book.useMap);
    EXPECT_EQ(static_cast<int>(book.treeLevels.size()), 260);

    // erase down to below HYBRID_ARRAY_RESUME (128) -> switches back to array
    for (int i = 0; i < 200; ++i) book.erase(1000 + i);
    EXPECT_FALSE(book.useMap);
    EXPECT_EQ(book.size(), 60);
    EXPECT_EQ(book.find(1259)->qty, 1);
}

TEST(HybridLevelBook, Clear) {
    HybridLevelBook book;
    for (int i = 0; i < 300; ++i) book.insert(1000 + i, 1);
    EXPECT_TRUE(book.useMap);
    book.clear();
    EXPECT_FALSE(book.useMap);
    EXPECT_EQ(book.size(), 0);
}
