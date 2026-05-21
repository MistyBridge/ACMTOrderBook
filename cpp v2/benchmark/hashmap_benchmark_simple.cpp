// =====================================================================
//  hashmap_benchmark_simple.cpp — 简化版哈希表基准测试
//
//  仅测试 std::unordered_map，用于验证测试框架
//  完整版需要下载第三方库
//
//  编译：cl /std:c++17 /O2 /arch:AVX2 /EHsc hashmap_benchmark_simple.cpp
// =====================================================================

#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include <unordered_map>
#include <cstdint>

// 测试数据结构
struct TestOrder {
    uint64_t seqNum;
    int32_t  price;
    int32_t  qty;
    uint8_t  side;
    uint8_t  orderType;
    uint16_t padding;
};
static_assert(sizeof(TestOrder) == 16, "TestOrder should be 16 bytes");

// 计时器
class Timer {
    std::chrono::high_resolution_clock::time_point start_;
    std::string name_;
public:
    Timer(const std::string& name) : name_(name) {
        start_ = std::chrono::high_resolution_clock::now();
    }
    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        std::cout << std::setw(35) << std::left << name_
                  << std::setw(12) << std::right << us << " μs ("
                  << std::fixed << std::setprecision(2)
                  << us / 1000.0 << " ms)" << std::endl;
    }
};

int main() {
    std::cout << "=====================================================================" << std::endl;
    std::cout << "  简化版哈希表基准测试" << std::endl;
    std::cout << "  测试规模: 500,000 次操作" << std::endl;
    std::cout << "=====================================================================" << std::endl;
    std::cout << std::endl;

    const size_t MAP_SIZE = 500000;
    const size_t OP_COUNT = 500000;

    // 生成顺序 key
    std::vector<uint64_t> keys(MAP_SIZE);
    for (size_t i = 0; i < MAP_SIZE; ++i) {
        keys[i] = 1000000 + i;
    }

    // 生成随机 key（用于查找）
    std::vector<uint64_t> randomKeys(OP_COUNT);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(1000000, 1000000 + MAP_SIZE - 1);
    for (auto& k : randomKeys) {
        k = dist(rng);
    }

    std::cout << "---------------------------------------------------------------------" << std::endl;
    std::cout << "  测试 1: std::unordered_map" << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;

    // 插入测试
    std::unordered_map<uint64_t, TestOrder> map;
    map.reserve(MAP_SIZE);
    {
        Timer t("Sequential Insert (500K)");
        for (size_t i = 0; i < MAP_SIZE; ++i) {
            map[keys[i]] = TestOrder{keys[i], 1000, 100, 0, 0, 0};
        }
    }
    std::cout << "  Map size: " << map.size() << std::endl;

    // 查找测试
    {
        volatile size_t found = 0;
        Timer t("Random Find (500K)");
        for (const auto& k : randomKeys) {
            auto it = map.find(k);
            if (it != map.end()) ++found;
        }
        std::cout << "  Found: " << found << " / " << OP_COUNT << std::endl;
    }

    // 删除测试
    {
        Timer t("Random Erase (500K)");
        for (const auto& k : randomKeys) {
            map.erase(k);
        }
    }
    std::cout << "  Remaining: " << map.size() << std::endl;

    std::cout << std::endl;
    std::cout << "=====================================================================" << std::endl;
    std::cout << "  测试完成" << std::endl;
    std::cout << "=====================================================================" << std::endl;

    return 0;
}