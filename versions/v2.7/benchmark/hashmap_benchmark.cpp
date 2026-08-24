// =====================================================================
//  hashmap_benchmark.cpp — 平铺哈希表选型基准测试
//
//  目标：比较 std::unordered_map 与三种平铺哈希表的性能
//  测试场景：模拟 orderMap 的典型操作模式
//    - 插入：新委托进入订单簿
//    - 查找：成交/撤单时定位委托
//    - 删除：撤单/完全成交后移除
//    - 混合负载：模拟真实交易场景
//
//  编译：cl /std:c++17 /O2 /arch:AVX2 /EHsc hashmap_benchmark.cpp
// =====================================================================

#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <functional>
#include <string>
#include <cstdint>
#include <unordered_map>

// =====================================================================
//  候选哈希表库（header-only）
//  通过 CMakeLists.txt 选项控制编译
// =====================================================================

#ifdef USE_ROBIN_HOOD
#include "robin_hood.h"
// robin_hood::unordered_map 兼容 std::unordered_map 接口
#endif

#ifdef USE_PHMAP
#include "absl/container/flat_hash_map.h"
// phmap::flat_hash_map 兼容 std::unordered_map 接口
#endif

#ifdef USE_UNORDERED_DENSE
#include "ankerl/unordered_dense.h"
// ankerl::unordered_dense::map 兼容 std::unordered_map 接口
#endif

// =====================================================================
//  测试数据结构（模拟 ObOrder）
// =====================================================================
struct TestOrder {
    uint64_t seqNum;      // 序列号（key）
    int32_t  price;       // 价格
    int32_t  qty;         // 数量
    uint8_t  side;        // 买卖方向
    uint8_t  orderType;   // 订单类型
    uint8_t  padding[2];  // 对齐填充
};
static_assert(sizeof(TestOrder) == 24, "TestOrder should be 24 bytes");

// =====================================================================
//  基准测试框架
// =====================================================================
class BenchmarkTimer {
    std::chrono::high_resolution_clock::time_point start_;
    std::string name_;
public:
    BenchmarkTimer(const std::string& name) : name_(name) {
        start_ = std::chrono::high_resolution_clock::now();
    }
    ~BenchmarkTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
        std::cout << std::setw(30) << std::left << name_
                  << std::setw(12) << std::right << duration.count() << " μs"
                  << std::endl;
    }
};

// 生成测试数据
std::vector<uint64_t> generateSequentialKeys(size_t count) {
    std::vector<uint64_t> keys(count);
    for (size_t i = 0; i < count; ++i) {
        keys[i] = 1000000 + i;  // 模拟序列号
    }
    return keys;
}

std::vector<uint64_t> generateRandomKeys(size_t count, uint64_t maxKey) {
    std::vector<uint64_t> keys(count);
    std::mt19937_64 rng(42);  // 固定种子，保证可复现
    std::uniform_int_distribution<uint64_t> dist(1000000, maxKey);
    for (auto& key : keys) {
        key = dist(rng);
    }
    return keys;
}

// 混合操作序列（模拟真实交易场景）
struct MixedOp {
    enum Type { INSERT, FIND, ERASE };
    Type type;
    uint64_t key;
};

std::vector<MixedOp> generateMixedOps(size_t count, double insertRatio = 0.3, double eraseRatio = 0.2) {
    std::vector<MixedOp> ops(count);
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    uint64_t nextKey = 1000000;
    std::vector<uint64_t> activeKeys;

    for (auto& op : ops) {
        double r = dist(rng);
        if (r < insertRatio || activeKeys.empty()) {
            // 插入操作
            op.type = MixedOp::INSERT;
            op.key = nextKey++;
            activeKeys.push_back(op.key);
        } else if (r < insertRatio + eraseRatio && !activeKeys.empty()) {
            // 删除操作
            op.type = MixedOp::ERASE;
            std::uniform_int_distribution<size_t> idxDist(0, activeKeys.size() - 1);
            size_t idx = idxDist(rng);
            op.key = activeKeys[idx];
            activeKeys.erase(activeKeys.begin() + idx);
        } else {
            // 查找操作
            op.type = MixedOp::FIND;
            if (!activeKeys.empty()) {
                std::uniform_int_distribution<size_t> idxDist(0, activeKeys.size() - 1);
                op.key = activeKeys[idxDist(rng)];
            } else {
                op.key = nextKey++;  // 查找不存在的 key
            }
        }
    }
    return ops;
}

// =====================================================================
//  基准测试用例
// =====================================================================

// 测试 1: 顺序插入性能
template<typename MapType>
void benchSequentialInsert(MapType& map, const std::vector<uint64_t>& keys, const std::string& name) {
    BenchmarkTimer timer(name + " - Sequential Insert");
    for (size_t i = 0; i < keys.size(); ++i) {
        map[keys[i]] = TestOrder{keys[i], static_cast<int32_t>(1000 + (i % 100)), 100, 0, 0, {0, 0}};
    }
}

// 测试 2: 随机查找性能
template<typename MapType>
void benchRandomFind(const MapType& map, const std::vector<uint64_t>& keys, const std::string& name) {
    BenchmarkTimer timer(name + " - Random Find");
    volatile size_t found = 0;  // 防止优化掉查找
    for (const auto& key : keys) {
        auto it = map.find(key);
        if (it != map.end()) {
            ++found;
        }
    }
    (void)found;
}

// 测试 3: 随机删除性能
template<typename MapType>
void benchRandomErase(MapType& map, const std::vector<uint64_t>& keys, const std::string& name) {
    BenchmarkTimer timer(name + " - Random Erase");
    for (const auto& key : keys) {
        map.erase(key);
    }
}

// 测试 4: 混合负载性能
template<typename MapType>
void benchMixedOps(MapType& map, const std::vector<MixedOp>& ops, const std::string& name) {
    BenchmarkTimer timer(name + " - Mixed Ops");
    for (const auto& op : ops) {
        switch (op.type) {
            case MixedOp::INSERT:
                map[op.key] = TestOrder{op.key, 1000, 100, 0, 0, {0, 0}};
                break;
            case MixedOp::FIND:
                map.find(op.key);
                break;
            case MixedOp::ERASE:
                map.erase(op.key);
                break;
        }
    }
}

// 测试 5: 内存占用估算
template<typename MapType>
size_t estimateMemoryUsage(const MapType& map, const std::string& name) {
    // 简化估算：实际应使用自定义分配器精确测量
    size_t count = map.size();
    size_t bucketCount = 0;

    // 对于 std::unordered_map，可以获取 bucket_count
    if constexpr (std::is_same_v<MapType, std::unordered_map<uint64_t, TestOrder>>) {
        bucketCount = map.bucket_count();
    }

    // 估算：每个元素约 16 bytes (key) + 16 bytes (value) + 8 bytes (pointer) = 40 bytes
    // 加上 bucket 开销
    size_t estimatedBytes = count * 40 + bucketCount * 8;

    std::cout << std::setw(30) << std::left << name + " - Memory Est."
              << std::setw(12) << std::right << estimatedBytes / 1024 << " KB"
              << " (" << count << " elements)" << std::endl;

    return estimatedBytes;
}

// 特化 robin_hood
#ifdef USE_ROBIN_HOOD
template<>
size_t estimateMemoryUsage<robin_hood::unordered_map<uint64_t, TestOrder>>(
    const robin_hood::unordered_map<uint64_t, TestOrder>& map, const std::string& name) {
    size_t count = map.size();
    // robin_hood 使用开放寻址，内存布局不同
    size_t estimatedBytes = count * 48;  // 估算值

    std::cout << std::setw(30) << std::left << name + " - Memory Est."
              << std::setw(12) << std::right << estimatedBytes / 1024 << " KB"
              << " (" << count << " elements)" << std::endl;

    return estimatedBytes;
}
#endif

// =====================================================================
//  主测试流程
// =====================================================================
int main() {
    std::cout << "=====================================================================" << std::endl;
    std::cout << "  平铺哈希表选型基准测试" << std::endl;
    std::cout << "  测试规模: 1,000,000 次操作" << std::endl;
    std::cout << "=====================================================================" << std::endl;
    std::cout << std::endl;

    const size_t OP_COUNT = 1000000;
    const size_t MAP_SIZE = 500000;  // 预填充大小

    // 生成测试数据
    auto sequentialKeys = generateSequentialKeys(MAP_SIZE);
    auto randomKeys = generateRandomKeys(OP_COUNT, MAP_SIZE + 1000000);
    auto mixedOps = generateMixedOps(OP_COUNT);

    std::cout << "---------------------------------------------------------------------" << std::endl;
    std::cout << "  测试 1: std::unordered_map (基准)" << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;

    {
        std::unordered_map<uint64_t, TestOrder> map;
        map.reserve(MAP_SIZE);

        benchSequentialInsert(map, sequentialKeys, "std::unordered_map");
        benchRandomFind(map, randomKeys, "std::unordered_map");
        estimateMemoryUsage(map, "std::unordered_map");

        // 重建 map 用于删除测试
        map.clear();
        map.reserve(MAP_SIZE);
        for (size_t i = 0; i < MAP_SIZE; ++i) {
            map[sequentialKeys[i]] = TestOrder{sequentialKeys[i], 1000, 100, 0, 0, {0, 0}};
        }
        benchRandomErase(map, randomKeys, "std::unordered_map");
    }

    std::cout << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
    std::cout << "  测试 2: robin_hood::unordered_map" << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
#ifdef USE_ROBIN_HOOD
    {
        robin_hood::unordered_map<uint64_t, TestOrder> map;
        map.reserve(MAP_SIZE);

        benchSequentialInsert(map, sequentialKeys, "robin_hood");
        benchRandomFind(map, randomKeys, "robin_hood");
        estimateMemoryUsage(map, "robin_hood");

        map.clear();
        map.reserve(MAP_SIZE);
        for (size_t i = 0; i < MAP_SIZE; ++i) {
            map[sequentialKeys[i]] = TestOrder{sequentialKeys[i], 1000, 100, 0, 0, {0, 0}};
        }
        benchRandomErase(map, randomKeys, "robin_hood");
    }
#else
    std::cout << "  [跳过 - 未启用 USE_ROBIN_HOOD]" << std::endl;
#endif

    std::cout << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
    std::cout << "  测试 3: phmap::flat_hash_map" << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
#ifdef USE_PHMAP
    {
        phmap::flat_hash_map<uint64_t, TestOrder> map;
        map.reserve(MAP_SIZE);

        benchSequentialInsert(map, sequentialKeys, "phmap::flat_hash_map");
        benchRandomFind(map, randomKeys, "phmap::flat_hash_map");
        estimateMemoryUsage(map, "phmap::flat_hash_map");

        map.clear();
        map.reserve(MAP_SIZE);
        for (size_t i = 0; i < MAP_SIZE; ++i) {
            map[sequentialKeys[i]] = TestOrder{sequentialKeys[i], 1000, 100, 0, 0, {0, 0}};
        }
        benchRandomErase(map, randomKeys, "phmap::flat_hash_map");
    }
#else
    std::cout << "  [跳过 - 未启用 USE_PHMAP]" << std::endl;
#endif

    std::cout << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
    std::cout << "  测试 4: ankerl::unordered_dense::map" << std::endl;
    std::cout << "---------------------------------------------------------------------" << std::endl;
#ifdef USE_UNORDERED_DENSE
    {
        ankerl::unordered_dense::map<uint64_t, TestOrder> map;
        map.reserve(MAP_SIZE);

        benchSequentialInsert(map, sequentialKeys, "ankerl::unordered_dense");
        benchRandomFind(map, randomKeys, "ankerl::unordered_dense");
        estimateMemoryUsage(map, "ankerl::unordered_dense");

        map.clear();
        map.reserve(MAP_SIZE);
        for (size_t i = 0; i < MAP_SIZE; ++i) {
            map[sequentialKeys[i]] = TestOrder{sequentialKeys[i], 1000, 100, 0, 0, {0, 0}};
        }
        benchRandomErase(map, randomKeys, "ankerl::unordered_dense");
    }
#else
    std::cout << "  [跳过 - 未启用 USE_UNORDERED_DENSE]" << std::endl;
#endif

    std::cout << std::endl;
    std::cout << "=====================================================================" << std::endl;
    std::cout << "  混合负载测试（模拟真实交易场景）" << std::endl;
    std::cout << "  插入:30% | 查找:50% | 删除:20%" << std::endl;
    std::cout << "=====================================================================" << std::endl;

    {
        std::unordered_map<uint64_t, TestOrder> map;
        map.reserve(MAP_SIZE);

        // 预填充
        for (size_t i = 0; i < MAP_SIZE / 2; ++i) {
            map[sequentialKeys[i]] = TestOrder{sequentialKeys[i], 1000, 100, 0, 0, {0, 0}};
        }

        benchMixedOps(map, mixedOps, "std::unordered_map");
    }

#ifdef USE_ROBIN_HOOD
    {
        robin_hood::unordered_map<uint64_t, TestOrder> map;
        map.reserve(MAP_SIZE);
        for (size_t i = 0; i < MAP_SIZE / 2; ++i) {
            map[sequentialKeys[i]] = TestOrder{sequentialKeys[i], 1000, 100, 0, 0, {0, 0}};
        }
        benchMixedOps(map, mixedOps, "robin_hood");
    }
#endif

#ifdef USE_PHMAP
    {
        phmap::flat_hash_map<uint64_t, TestOrder> map;
        map.reserve(MAP_SIZE);
        for (size_t i = 0; i < MAP_SIZE / 2; ++i) {
            map[sequentialKeys[i]] = TestOrder{sequentialKeys[i], 1000, 100, 0, 0, {0, 0}};
        }
        benchMixedOps(map, mixedOps, "phmap::flat_hash_map");
    }
#endif

#ifdef USE_UNORDERED_DENSE
    {
        ankerl::unordered_dense::map<uint64_t, TestOrder> map;
        map.reserve(MAP_SIZE);
        for (size_t i = 0; i < MAP_SIZE / 2; ++i) {
            map[sequentialKeys[i]] = TestOrder{sequentialKeys[i], 1000, 100, 0, 0, {0, 0}};
        }
        benchMixedOps(map, mixedOps, "ankerl::unordered_dense");
    }
#endif

    std::cout << std::endl;
    std::cout << "=====================================================================" << std::endl;
    std::cout << "  测试完成" << std::endl;
    std::cout << "=====================================================================" << std::endl;

    return 0;
}