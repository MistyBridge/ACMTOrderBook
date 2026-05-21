// test_ankerl.cpp — 快速验证 ankerl::unordered_dense 性能
// 编译: cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /I..\third_party\unordered_dense\include test_ankerl.cpp

#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include <unordered_map>
#include "../third_party/unordered_dense/include/ankerl/unordered_dense.h"

struct TestOrder {
    uint64_t seqNum;
    int32_t  price;
    int32_t  qty;
    uint8_t  side;
    uint8_t  orderType;
    uint16_t padding;
};

int main() {
    std::cout << "=====================================================================" << std::endl;
    std::cout << "  ankerl::unordered_dense vs std::unordered_map 性能对比" << std::endl;
    std::cout << "=====================================================================" << std::endl;
    std::cout << std::endl;

    const size_t N = 500000;
    std::vector<uint64_t> keys(N);
    for (size_t i = 0; i < N; ++i) keys[i] = 1000000 + i;

    std::vector<uint64_t> randKeys(N);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(1000000, 1000000 + N - 1);
    for (auto& k : randKeys) k = dist(rng);

    // --- std::unordered_map ---
    std::cout << "---------------------------------------------------------------------" << std::endl;
    std::cout << "  std::unordered_map" << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
    {
        std::unordered_map<uint64_t, TestOrder> map;
        map.reserve(N);

        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < N; ++i)
            map[keys[i]] = TestOrder{keys[i], 1000, 100, 0, 0, 0};
        auto t1 = std::chrono::high_resolution_clock::now();
        std::cout << "  Insert: " << std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count() << " us" << std::endl;

        volatile size_t found = 0;
        t0 = std::chrono::high_resolution_clock::now();
        for (auto& k : randKeys) { auto it = map.find(k); if (it != map.end()) ++found; }
        t1 = std::chrono::high_resolution_clock::now();
        std::cout << "  Find:   " << std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count() << " us" << std::endl;

        t0 = std::chrono::high_resolution_clock::now();
        for (auto& k : randKeys) map.erase(k);
        t1 = std::chrono::high_resolution_clock::now();
        std::cout << "  Erase:  " << std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count() << " us" << std::endl;
    }

    // --- ankerl::unordered_dense::map ---
    std::cout << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
    std::cout << "  ankerl::unordered_dense::map" << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
    {
        ankerl::unordered_dense::map<uint64_t, TestOrder> map;
        map.reserve(N);

        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < N; ++i)
            map[keys[i]] = TestOrder{keys[i], 1000, 100, 0, 0, 0};
        auto t1 = std::chrono::high_resolution_clock::now();
        std::cout << "  Insert: " << std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count() << " us" << std::endl;

        volatile size_t found = 0;
        t0 = std::chrono::high_resolution_clock::now();
        for (auto& k : randKeys) { auto it = map.find(k); if (it != map.end()) ++found; }
        t1 = std::chrono::high_resolution_clock::now();
        std::cout << "  Find:   " << std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count() << " us" << std::endl;

        t0 = std::chrono::high_resolution_clock::now();
        for (auto& k : randKeys) map.erase(k);
        t1 = std::chrono::high_resolution_clock::now();
        std::cout << "  Erase:  " << std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count() << " us" << std::endl;
    }

    std::cout << std::endl;
    std::cout << "=====================================================================" << std::endl;
    std::cout << "  测试完成" << std::endl;
    std::cout << "=====================================================================" << std::endl;
    return 0;
}