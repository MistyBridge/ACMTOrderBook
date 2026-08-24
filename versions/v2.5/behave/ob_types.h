#pragma once
#include <cstdint>
#include <cstring>       // std::memmove (HybridLevelBook)
#include <type_traits>   // std::is_trivially_copyable_v
#include "../tool/axsbe_base.h"
#include "../tool/axsbe_order.h"
#include "../tool/axsbe_exe.h"

// =====================================================================
//  [v2优化] 分支预测宏 — 引导 CPU 分支预测器，减少分支预测失败惩罚
//  在热路径中用 LIKELY()/UNLIKELY() 包裹条件表达式
// =====================================================================
#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
#endif

// =====================================================================
//  内部数据结构（对应 Python axob.py 中的 ob_order/ob_exec/
//               ob_cancel/level_node）
//  所有价格/数量统一使用内部精度（股票2位小数）
//
//  [v2优化] ObOrder/ObExec 字段重排：按 8B→4B→1B 降序排列，
//          消除结构体内部 padding，减少内存占用并提升缓存命中率。
// =====================================================================

enum class CageType : uint8_t { NONE = 0, CYB = 1 };

enum class CageSide : uint8_t { NONE = 0, BID = 1, ASK = 2 };

enum class AXSignal : uint8_t {
    OPENCALL_END   = 1,
    AMTRADING_BGN  = 2,
    AMTRADING_END  = 3,
    PMTRADING_END  = 5,
    ALL_END        = 6,
    VB_BGN         = 7,   // 进入波动性中断
    VB_END         = 8,   // 退出波动性中断
};

// ---- 市场子类型（根据 SecurityID 自动判断）----
enum class MarketSubType : uint8_t {
    SZSE_STK_MB    = 0,   // 深圳主板 000/001
    SZSE_STK_SME   = 1,   // 中小板 002
    SZSE_STK_GEM   = 2,   // 创业板 300
    SZSE_STK_B     = 3,   // B股 200
    SZSE_KZZ       = 4,   // 可转债 127/128
    SZSE_OTHERS    = 5,
    SSE            = 6,   // 上交所
};

inline MarketSubType marketSubType(SecurityIDSource src, int securityID) {
    if (src == SecurityIDSource_SZSE) {
        int prefix = securityID / 1000;
        if (prefix == 0   || prefix == 1)   return MarketSubType::SZSE_STK_MB;
        if (prefix == 2)                     return MarketSubType::SZSE_STK_SME;
        if (prefix == 300 || prefix == 301)  return MarketSubType::SZSE_STK_GEM;
        if (prefix == 200)                   return MarketSubType::SZSE_STK_B;
        if (prefix == 127 || prefix == 128)  return MarketSubType::SZSE_KZZ;
        return MarketSubType::SZSE_OTHERS;
    }
    return MarketSubType::SSE;
}

// ---- 市场常量（由第一条快照初始化）----
struct MarketInfo {
    int32_t  PrevClosePx   = 0;
    int32_t  UpLimitPx     = 0;
    int32_t  DnLimitPx     = 0;
    int32_t  UpLimitPrice  = 0;
    int32_t  DnLimitPrice  = 0;
    uint16_t ChannelNo     = 0;
    uint64_t YYMMDD        = 0;
};

// ---- ob_order：内部订单（精度已转换）----
// [v2优化] 字段按大小降序排列，消除 padding：
//   v1布局：uint64(8) + int32(4) + int32(4) + int8(1)+pad(3) + int8(1)+pad(3) + bool(1)+pad(7) + uint64(8) = 40B
//   v2布局：uint64(8) + uint64(8) + int32(4) + int32(4) + int8(1) + int8(1) + bool(1) + pad(1) = 32B
//   节省 8B/对象，对 82K 订单 → 节省 ~640KB，且每条 cache line 多装 2 个对象
struct ObOrder {
    uint64_t applSeqNum;       // 8B  offset=0
    uint64_t TransactTime = 0; // 8B  offset=8
    int32_t  price;            // 4B  offset=16
    int32_t  qty;              // 4B  offset=20
    Side     side;             // 1B  offset=24
    OrdType  type;             // 1B  offset=25
    bool     traded = false;   // 1B  offset=26
    // 1B 尾部 padding → 28B → 对齐到 8B → 32B total

    ObOrder() : applSeqNum(0), TransactTime(0), price(0), qty(0),
                side(Side::UNKNOWN), type(OrdType::UNKNOWN) {}

    ObOrder(const AxsbeOrder& raw, InstrumentType instType)
        : applSeqNum(raw.ApplSeqNum), TransactTime(raw.TransactTime),
          qty(static_cast<int32_t>(raw.OrderQty))
    {
        // [v2优化] 分支预测提示：价格溢出是极罕见事件
        if (UNLIKELY(raw.Price == ORDER_PRICE_OVERFLOW)) {
            price = PRICE_MAXIMUM;
        } else {
            // 精度转换（深交所是热路径）
            if (LIKELY(raw.secSrc == SecurityIDSource_SZSE)) {
                if (LIKELY(instType == InstrumentType::STOCK))
                    price = static_cast<int32_t>(raw.Price / SZSE_STOCK_PRICE_RD);
                else if (instType == InstrumentType::FUND)
                    price = static_cast<int32_t>(raw.Price / SZSE_FUND_PRICE_RD);
                else if (instType == InstrumentType::KZZ)
                    price = static_cast<int32_t>(raw.Price / SZSE_KZZ_PRICE_RD);
                else
                    price = 0;
            } else if (raw.secSrc == SecurityIDSource_SSE) {
                if (instType == InstrumentType::STOCK)
                    price = static_cast<int32_t>(raw.Price / SSE_STOCK_PRICE_RD);
                else
                    price = 0;
            } else {
                price = 0;
            }
        }

        side = raw.isBuy() ? Side::BID : (raw.isSell() ? Side::ASK : Side::UNKNOWN);

        if (raw.isLimit())       type = OrdType::LIMIT;
        else if (raw.isMarket()) type = OrdType::MARKET;
        else if (raw.isSideOptimal()) type = OrdType::SIDE;
        else                     type = OrdType::UNKNOWN;
    }
};

// ---- ob_exec：内部成交 ----
// [v2优化] 字段按大小降序排列：
//   v2布局：uint64(8)*3 + int32(4)*2 + int8(1)+pad(3) = 40B
struct ObExec {
    uint64_t BidApplSeqNum;       // 8B offset=0
    uint64_t OfferApplSeqNum;     // 8B offset=8
    uint64_t TransactTime;        // 8B offset=16
    int32_t  LastPx;              // 4B offset=24
    int32_t  LastQty;             // 4B offset=28
    TPM      tradingPhaseMarket;  // 1B offset=32 → pad(3) → 40B total

    ObExec(const AxsbeExe& raw, InstrumentType instType)
        : BidApplSeqNum(raw.BidApplSeqNum),
          OfferApplSeqNum(raw.OfferApplSeqNum),
          TransactTime(raw.TransactTime),
          LastQty(static_cast<int32_t>(raw.LastQty)),
          tradingPhaseMarket(TPM::Unknown)
    {
        // [v2优化] 分支预测提示
        if (LIKELY(raw.secSrc == SecurityIDSource_SZSE)) {
            if (LIKELY(instType == InstrumentType::STOCK))
                LastPx = static_cast<int32_t>(raw.LastPx / SZSE_STOCK_PRICE_RD);
            else if (instType == InstrumentType::FUND)
                LastPx = static_cast<int32_t>(raw.LastPx / SZSE_FUND_PRICE_RD);
            else if (instType == InstrumentType::KZZ)
                LastPx = static_cast<int32_t>(raw.LastPx / SZSE_KZZ_PRICE_RD);
            else
                LastPx = 0;
        } else if (raw.secSrc == SecurityIDSource_SSE) {
            if (instType == InstrumentType::STOCK)
                LastPx = static_cast<int32_t>(raw.LastPx / SSE_STOCK_PRICE_RD);
            else
                LastPx = 0;
        } else {
            LastPx = 0;
        }
    }
};

// ---- ob_cancel：内部撤单 ----
// [v2优化] 字段按大小降序排列
struct ObCancel {
    uint64_t applSeqNum;      // 8B offset=0
    uint64_t TransactTime;    // 8B offset=8
    int32_t  qty;             // 4B offset=16
    int32_t  price;           // 4B offset=20
    Side     side;            // 1B offset=24 → pad(3) → 32B total
};

// ---- level_node：价格档位 ----
struct LevelNode {
    int32_t price = 0;
    int32_t qty   = 0;
    LevelNode() = default;
    LevelNode(int32_t p, int32_t q) : price(p), qty(q) {}
};

// ---- 工具函数 ----
inline int32_t fmtPriceInter2Snap(int32_t price, InstrumentType instType, SecurityIDSource src) {
    if (src == SecurityIDSource_SZSE) {
        if (instType == InstrumentType::STOCK)
            return price * (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_STOCK_PRECISION);
        if (instType == InstrumentType::FUND)
            return price * (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_FUND_PRECISION);
        if (instType == InstrumentType::KZZ)
            return price * (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_KZZ_PRECISION);
    } else if (src == SecurityIDSource_SSE) {
        if (instType == InstrumentType::STOCK)
            return price * (PRICE_SSE_PRECISION / PRICE_INTER_STOCK_PRECISION);
    }
    return price;
}

inline int32_t clipInt32(int64_t x) {
    if (x > INT32_MAX) return INT32_MAX;
    if (x < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(x);
}

// =====================================================================
//  HybridLevelBook — 混合价格档位簿（替代 std::map<int32_t, LevelNode>）
//
//  根据档位数量动态选择存储方式：
//    n ≤ 256 : 排序数组（连续内存，CPU 预取命中，无指针追逐）
//    n > 256 : std::map 红黑树（O(log n) insert/erase，避免大数组 memmove）
//
//  交叉点分析：
//    - 数组 find:  O(n) 但顺序访问完美预取，n=256 时 ~100ns
//    - 树 find:    O(log n) 但每次指针追逐可能 cache miss，n=256 时 ~400-800ns
//    - 数组 insert: memmove ~128 节点 ≈ 200-400ns（顺序写，带宽受限）
//    - 树 insert:  8 次指针追逐 + 旋转 ≈ 400-800ns（随机访问，cache miss 受限）
//    → n ≤ 256 时数组全面胜出
//
//  切换策略：
//    - insert 时 array 模式且 count 达到 256 → 迁移到 map
//    - erase 时 map 模式且 count 降至 128（2× 滞后防抖动）→ 迁移回 array
//
//  接口兼容性：
//    - levels[] / count 直接访问（仅 array 模式安全，map 模式由迁移保证）
//    - find() / insert() / erase() / modifyQty() 模式无关
//    - for_each() / rfor_each() 模式无关的遍历接口
//    - toSortedArray() / toSortedArrayDesc() map 模式转数组快照
// =====================================================================

#include <map>  // std::map 用于大 n 回退

// [v2.6] 热点路径性能计数器（编译时可通过 #define DISABLE_PROFILING 关闭）
#define DISABLE_PROFILING  // 移除 profiling 开销以获得最佳性能
#ifndef DISABLE_PROFILING
struct HotPathStats {
    uint64_t find_cycles     = 0;
    uint64_t find_count      = 0;
    uint64_t insert_cycles   = 0;
    uint64_t insert_count    = 0;
    uint64_t erase_cycles    = 0;
    uint64_t erase_count     = 0;
    uint64_t modifyQty_cycles= 0;
    uint64_t modifyQty_count = 0;
    uint64_t memmove_bytes   = 0;
    uint64_t genSnap_cycles  = 0;
    uint64_t genSnap_count   = 0;
    uint64_t fmtPx_cycles    = 0;
    uint64_t fmtPx_count     = 0;
    uint64_t orderMap_cycles = 0;
    uint64_t orderMap_count  = 0;

    void reset() { *this = HotPathStats{}; }
    void print() const {
        printf("[HotPath] find: %llu calls, avg %.1f cycles\n",
               (unsigned long long)find_count,
               find_count ? (double)find_cycles / find_count : 0.0);
        printf("[HotPath] insert: %llu calls, avg %.1f cycles, memmove=%llu bytes\n",
               (unsigned long long)insert_count,
               insert_count ? (double)insert_cycles / insert_count : 0.0,
               (unsigned long long)memmove_bytes);
        printf("[HotPath] erase: %llu calls, avg %.1f cycles\n",
               (unsigned long long)erase_count,
               erase_count ? (double)erase_cycles / erase_count : 0.0);
        printf("[HotPath] genSnap: %llu calls, avg %.1f cycles\n",
               (unsigned long long)genSnap_count,
               genSnap_count ? (double)genSnap_cycles / genSnap_count : 0.0);
        printf("[HotPath] fmtPx: %llu calls, avg %.1f cycles\n",
               (unsigned long long)fmtPx_count,
               fmtPx_count ? (double)fmtPx_cycles / fmtPx_count : 0.0);
        printf("[HotPath] orderMap: %llu ops, avg %.1f cycles\n",
               (unsigned long long)orderMap_count,
               orderMap_count ? (double)orderMap_cycles / orderMap_count : 0.0);
    }
};
inline HotPathStats g_hotStats;
#define PROF_CYC_START(var) uint64_t var = __rdtsc()
#define PROF_CYC_END(stats, field, count_field, start) do { (stats).field += __rdtsc() - (start); (stats).count_field++; } while(0)
#else
struct HotPathStats {
    void reset() {}
    void print() const {}
};
inline HotPathStats g_hotStats;
#define PROF_CYC_START(var) ((void)0)
#define PROF_CYC_END(stats, field, count_field, start) ((void)0)
#endif

static constexpr int HYBRID_ARRAY_MAX    = 256;   // 超过此值切 map
static constexpr int HYBRID_ARRAY_RESUME = 128;   // 低于此值切回 array

struct HybridLevelBook {
    bool useMap = false;  // false = 排序数组模式, true = std::map 模式

    // ---- 排序数组模式（直接访问，兼容现有代码）----
    LevelNode levels[HYBRID_ARRAY_MAX];
    int       count = 0;

    // ---- std::map 模式（大 n 回退）----
    std::map<int32_t, LevelNode> treeLevels;

    // ================================================================
    //  find() — 查找指定价格的档位（模式无关）
    // ================================================================
    // [v2.6] 二分查找优化：O(n) → O(log n)
    LevelNode* find(int32_t price) {
        if (LIKELY(!useMap)) {
            int lo = 0, hi = count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                if (levels[mid].price == price) return &levels[mid];
                else if (levels[mid].price < price) lo = mid + 1;
                else hi = mid - 1;
            }
            return nullptr;
        } else {
            auto it = treeLevels.find(price);
            return (it != treeLevels.end()) ? &it->second : nullptr;
        }
    }
    // [v2.6] 二分查找优化（const 版本）
    const LevelNode* find(int32_t price) const {
        if (LIKELY(!useMap)) {
            int lo = 0, hi = count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                if (levels[mid].price == price) return &levels[mid];
                else if (levels[mid].price < price) lo = mid + 1;
                else hi = mid - 1;
            }
            return nullptr;
        } else {
            auto it = treeLevels.find(price);
            return (it != treeLevels.end()) ? &it->second : nullptr;
        }
    }

    // ================================================================
    //  insert() — 插入新档位（维持升序排列，模式无关）
    // ================================================================
    LevelNode* insert(int32_t price, int32_t qty) {
        if (LIKELY(!useMap)) {
            if (UNLIKELY(count >= HYBRID_ARRAY_MAX)) {
                migrateToMap();
                auto [it, ok] = treeLevels.emplace(price, LevelNode(price, qty));
                return &it->second;
            }
            int lo = 0, hi = count;
            while (lo < hi) {
                int mid = lo + (hi - lo) / 2;
                if (levels[mid].price < price) lo = mid + 1;
                else hi = mid;
            }
            if (lo < count) {
                std::memmove(&levels[lo + 1], &levels[lo],
                             (count - lo) * sizeof(LevelNode));
            }
            levels[lo] = LevelNode(price, qty);
            count++;
            return &levels[lo];
        } else {
            auto [it, ok] = treeLevels.emplace(price, LevelNode(price, qty));
            return &it->second;
        }
    }

    // ================================================================
    //  erase() — 删除指定价格的档位（模式无关）
    // ================================================================
    void erase(int32_t price) {
        if (LIKELY(!useMap)) {
            // 二分查找要删除的位置
            int lo = 0, hi = count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                if (levels[mid].price == price) {
                    if (mid < count - 1) {
                        std::memmove(&levels[mid], &levels[mid + 1],
                                     (count - 1 - mid) * sizeof(LevelNode));
                    }
                    count--;
                    return;
                } else if (levels[mid].price < price) lo = mid + 1;
                else hi = mid - 1;
            }
        } else {
            treeLevels.erase(price);
            if (static_cast<int>(treeLevels.size()) <= HYBRID_ARRAY_RESUME) {
                migrateToArray();
            }
        }
    }

    // ================================================================
    //  modifyQty() — 修改指定价格档位的数量（模式无关，热路径）
    // ================================================================
    bool modifyQty(int32_t price, int32_t delta) {
        if (LIKELY(!useMap)) {
            int lo = 0, hi = count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                if (levels[mid].price == price) {
                    levels[mid].qty += delta;
                    return true;
                } else if (levels[mid].price < price) lo = mid + 1;
                else hi = mid - 1;
            }
            return false;
        } else {
            auto it = treeLevels.find(price);
            if (it != treeLevels.end()) {
                it->second.qty += delta;
                return true;
            }
            return false;
        }
    }

    // ================================================================
    //  size() — 当前档位数量（模式无关）
    // ================================================================
    int size() const {
        return useMap ? static_cast<int>(treeLevels.size()) : count;
    }

    // ================================================================
    //  模式无关遍历接口 — forward（卖方：小→大）
    //  void fn(const LevelNode& node)
    // ================================================================
    template<typename Fn>
    void for_each(Fn&& fn) const {
        if (LIKELY(!useMap)) {
            for (int i = 0; i < count; i++) fn(levels[i]);
        } else {
            for (auto& [p, l] : treeLevels) fn(l);
        }
    }

    // ================================================================
    //  模式无关遍历接口 — reverse（买方：大→小）
    // ================================================================
    template<typename Fn>
    void rfor_each(Fn&& fn) const {
        if (LIKELY(!useMap)) {
            for (int i = count - 1; i >= 0; i--) fn(levels[i]);
        } else {
            for (auto it = treeLevels.rbegin(); it != treeLevels.rend(); ++it)
                fn(it->second);
        }
    }

    // ================================================================
    //  迁移：array → map
    // ================================================================
    void migrateToMap() {
        treeLevels.clear();
        for (int i = 0; i < count; i++) {
            treeLevels.emplace(levels[i].price, levels[i]);
        }
        useMap = true;
    }

    // ================================================================
    //  迁移：map → array
    // ================================================================
    void migrateToArray() {
        count = 0;
        for (auto& [p, l] : treeLevels) {
            levels[count++] = l;
        }
        treeLevels.clear();
        useMap = false;
    }
};

// =====================================================================
//  静态断言：确保结构体可 trivially copy（内存池 / memcpy 安全）
// =====================================================================
static_assert(std::is_trivially_copyable_v<ObOrder>,
              "ObOrder must be trivially copyable for memory pool safety");
static_assert(std::is_trivially_copyable_v<ObExec>,
              "ObExec must be trivially copyable for memory pool safety");
static_assert(std::is_trivially_copyable_v<LevelNode>,
              "LevelNode must be trivially copyable");

// [v2优化] 结构体大小约束
static_assert(sizeof(ObOrder) <= 32,
              "ObOrder should fit in 32B after field reordering");
static_assert(sizeof(LevelNode) == 8,
              "LevelNode should be exactly 8B (two int32)");
// 注意：HybridLevelBook 包含 std::map，不能 trivially copy
