#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <type_traits>
#include <cstring>     // memmove (CompactLevelBook)
#include "ob_types.h"
#include "../tool/axsbe_base.h"
#include "../tool/axsbe_order.h"
#include "../tool/axsbe_exe.h"
#include "../tool/axsbe_snap_stock.h"
#include "../tool/msg_util.h"

// [v2优化] 内存池支持 — 通过编译宏开关，可在 CMakeLists.txt 中关闭
#ifndef USE_MEMORY_POOL
#define USE_MEMORY_POOL 1
#endif

#if USE_MEMORY_POOL
#include "../core/memory_pool.h"
#endif

// [v2.2优化] 平铺哈希表支持 — 通过编译宏开关选择哈希表实现
// 默认使用 std::unordered_map，可通过 CMakeLists.txt 切换到高性能平铺哈希表
#ifndef USE_FLAT_HASHMAP
#define USE_FLAT_HASHMAP 0
#endif

#if USE_FLAT_HASHMAP == 1
    // ankerl::unordered_dense::map — 单头文件，C++17，极致性能
    #include "../third_party/unordered_dense/include/ankerl/unordered_dense.h"
    template<typename K, typename V>
    using HashMap = ankerl::unordered_dense::map<K, V>;
#elif USE_FLAT_HASHMAP == 2
    // phmap::flat_hash_map — Google abseil，SIMD 优化
    #include "../third_party/abseil-cpp/absl/container/flat_hash_map.h"
    template<typename K, typename V>
    using HashMap = absl::flat_hash_map<K, V>;
#else
    // std::unordered_map — 标准库，兼容性最好
    template<typename K, typename V>
    using HashMap = std::unordered_map<K, V>;
#endif

// =====================================================================
//  AXOB — 订单簿重建引擎（v2 优化版）
//
//  实现分布在多个 .cpp 文件中：
//    axob_init.cpp     构造、工具函数
//    axob_order.cpp    委托处理
//    axob_trade.cpp    成交/撤单处理
//    axob_cage.cpp     创业板价格笼子
//    axob_snap.cpp     快照生成与验证
//
//  [v2优化] 变更清单：
//    1. orderMap 改为存储 ObOrder* 指针，配合 MemoryPool 减少堆分配
//    2. holdingOrder 从 unique_ptr 改为栈上对象，消除堆分配
//    3. 热字段（bidMaxPrice/askMinPrice/NumTrades等）集中到结构体头部
//    4. 冷字段（exportLevelAccess/lastSnap等）放在末尾
// =====================================================================

class AXOB {
public:
    // ==================== 股票标识 ====================
    int              SecurityID;
    SecurityIDSource secSrc;
    InstrumentType   instType;

    // [v2优化] 热字段集中到结构体开头，确保它们在同一个或相邻 cache line
    // 这些字段在每条消息处理中都会被读写

    // ==================== 最优档缓存（最高频读写）====================
    int64_t bidMaxPrice = 0;   // 内部 ×10^5
    int64_t askMinPrice = 0;
    int64_t bidMaxQty = 0;
    int64_t askMinQty = 0;

    // ==================== 市场统计（每笔成交都会更新）====================
    int64_t NumTrades         = 0;
    int64_t TotalVolumeTrade  = 0;
    int64_t TotalValueTrade   = 0;
    int64_t LastPx            = 0;   // 内部 ×10^5
    int64_t HighPx            = 0;
    int64_t LowPx             = 0;
    int64_t OpenPx            = 0;

    // ==================== 核心数据结构 ====================
    // [v2.2优化] orderMap 使用平铺哈希表，提升查找/插入/删除性能
    // USE_FLAT_HASHMAP=0: std::unordered_map (默认)
    // USE_FLAT_HASHMAP=1: ankerl::unordered_dense::map (推荐)
    // USE_FLAT_HASHMAP=2: phmap::flat_hash_map (Google abseil)
    HashMap<uint64_t, ObOrder*> orderMap;
    // [v2优化] HybridLevelBook 替代 std::map — 根据档位数量动态选择
    // n≤256: 排序数组（连续内存，缓存友好）
    // n>256: std::map（O(log n)，避免大数组 memmove）
    HybridLevelBook bidLevelBook;
    HybridLevelBook askLevelBook;

#if USE_MEMORY_POOL
    // [v2优化] 内存池：外部传入时复用，否则在构造时创建
    axob::core::MemoryPool<ObOrder>* orderPool_ = nullptr;
    bool ownPool_ = false;   // 是否自行创建（决定析构时是否释放）
#endif

    // ==================== 加权统计 ====================
    // *Value 为全簿 Σ(price × qty) 累计和: 统一定点 ×10^5 下单档乘积即可达 1e21,
    // 全簿求和更大, 必须 __int128 (实测 000001 买侧 1e8 股 × 10.54 元 = 1.05e19 已越 int64)。
    // *Size 为纯数量累计, int64 足够。
    int64_t  BidWeightSize    = 0;
    __int128 BidWeightValue   = 0;
    int64_t  AskWeightSize    = 0;
    __int128 AskWeightValue   = 0;
    int64_t  AskWeightSizeEx  = 0;
    __int128 AskWeightValueEx = 0;

    // ==================== 缓存单 ====================
    // [v2优化] 从 unique_ptr 改为栈上对象，省掉堆分配（ObOrder 仅 32B）
    ObOrder holdingOrder_;
    bool    hasHoldingOrder_ = false;

    // ==================== 时戳 ====================
    uint64_t currentIncTick = 0;
    uint64_t lastIncTransactTime = 0;  // 最后逐笔原始时间戳 (YYYYMMDDHHMMSSsss, 快照兜底滞后检测用)
    static constexpr uint64_t SZSE_TICK_CUT     = 1000000000ULL;
    static constexpr int      SZSE_TICK_MS_TAIL  = 10;
    static constexpr int      TIMESTAMP_BIT_SIZE = 28;  // 上交所精度1ms，最大28b

    // ==================== 交易阶段 ====================
    TPM tradingPhase = TPM::Starting;
    int volatilityBreakingEndTick = 0;

    // ==================== 市场子类型 ====================
    MarketSubType mktSubType = MarketSubType::SZSE_STK_MB;

    // ==================== 市场常量 ====================
    MarketInfo mktInfo;
    bool closePxReady = false;

    // ==================== 价格笼子 ====================
    CageType cageType = CageType::NONE;
    CageSide cageSide = CageSide::NONE;
    int64_t bidCageUpperExMinPrice = 0;
    int64_t bidCageUpperExMinQty   = 0;
    int64_t askCageLowerExMaxPrice = 0;
    int64_t askCageLowerExMaxQty   = 0;
    int64_t bidCageRefPx           = 0;
    int64_t askCageRefPx           = 0;
    bool    bidWaitingForCage      = false;
    bool    askWaitingForCage      = false;
    bool    cageSessionActive      = false;   // 连续竞价笼子是否已激活 (开盘后一次)

    // [v2优化] 冷字段放在末尾，不挤占热字段的 cache line

    // ==================== 导出开关（FPGA验证用）====================
    bool exportLevelAccess = false;

    // ==================== 快照兜底 (生产安全网, 双套+路由) ====================
    // 套A = 逐笔重建簿 (本引擎增量维护, 永不被快照覆盖);
    // 套B = 市场快照镜像 (最近一帧原样保留);
    // 输出按路由状态机切换: 默认 REBUILT; 触发条件进入 SNAPSHOT;
    // 套A 与套B 重新对齐 (十档全等+num_trades 窗口) 后自愈切回 REBUILT。
    // 触发: ①竞价时段 (9:25-9:30 / 14:57-15:00, 交易所此段为特殊展示口径)
    //       ②快照时间戳超过最后逐笔时间戳 (逐笔流断流/滞后, 正常时快照早 ~1s 不触发)
    //       ③连续竞价交叉簿异常
    enum class SnapRoute : uint8_t { REBUILT = 0, SNAPSHOT = 1 };
    bool snapFallbackEnabled = false;   // 校验闭环默认关闭 (审计口径), 生产打开
    SnapRoute snapRoute = SnapRoute::REBUILT;
    AxsbeSnapStock receivedSnapshot;     // 套B: 市场快照镜像
    bool hasReceivedSnapshot = false;
    void initMktInfoFromSnap(const AxsbeSnapStock& snap);
    void evalSnapRoute(const AxsbeSnapStock& snap);
    AxsbeSnapStock currentBook(int showLevelNb);  // 路由感知输出 (回放校验/生产查询共用)

    // ==================== 生产健壮性 ====================
    // orderMap 懒清理: 订单剩余量减至 0 (全成交) 且时间戳早于安全窗口后删除,
    // 按 "流内排序保证 + 60 秒安全窗口" 延迟删除, 控制长跑内存增长。
    bool orderMapCleanupEnabled = false;   // 回放默认关闭 (审计口径), 生产打开
    size_t cleanupCursor = 0;
    void cleanupOrderMap(uint64_t nowTransactTime);

    // 跨日脏数据过滤: 过滤前一交易日残留消息
    bool staleDataFilterEnabled = false;
    int64_t dayOfData = -1;               // 当前数据日 YYYYMMDD (首条消息设定)
    uint64_t staleFiltered = 0;           // 过滤计数

    // 健康计数: 可恢复错误路径不中断处理, 改为累计供监控采集
    struct HealthStat {
        uint64_t orderNotFound = 0;   // 成交/撤单回链失败的订单号
        uint64_t negLevelClear = 0;   // 负量档清除 (兜底模式削减越界保护)
        uint64_t snapRouteAdopt = 0;  // 快照兜底切换次数
        uint64_t cleanupErased  = 0;  // orderMap 清理删除的订单数
    } health;

    // ==================== 调试/测试 ====================
    int msgNb = 0;
    // [v2.2优化] lastSnap 从堆分配改为栈上对象，省掉每条消息的 delete/new (~50ns)
    AxsbeSnapStock lastSnap;

    // [v2.7优化] genSnap 延迟重建标记
    // genSnap() 只标记脏标志（~2ns），ensureSnap() 在需要时才完整重建（~225ns）
    bool snapNeedsUpdate_ = false;

    // ==================== 构造 ====================
#if USE_MEMORY_POOL
    AXOB(int securityID, SecurityIDSource src, InstrumentType type,
         axob::core::MemoryPool<ObOrder>* pool = nullptr);
#else
    AXOB(int securityID, SecurityIDSource src, InstrumentType type);
#endif
    ~AXOB();

    // ==================== 消息入口（重载分发）====================
    void onMsg(const AxsbeOrder& msg);
    void onMsg(const AxsbeExe&   msg);
    void onMsg(const AxsbeSnapStock& msg);
    // 跨日脏数据检测 (生产过滤, 回放关闭)
    bool isStaleData(uint64_t transactTime);
    void onMsg(AXSignal signal);

    // ==================== 委托处理（axob_order.cpp）====================
    void onOrder(const AxsbeOrder& rawOrder);
    void onLimitOrder(ObOrder& order);
    void insertOrder(const ObOrder& order, bool outOfCage = false);

    // ==================== 成交/撤单（axob_trade.cpp）====================
    void onExec(const AxsbeExe& rawExec);
    void onTrade(const ObExec& exec);
    void onCancel(const ObCancel& cancel);
    void tradeLimit(Side side, int64_t qty, uint64_t applSeqNum);
    void levelDequeue(Side side, int64_t price, int64_t qty, uint64_t applSeqNum);

    // ==================== 价格笼子（axob_cage.cpp）====================
    void openCage();
    void enterCage();
    // 进入连续竞价时, 把集合竞价期间挂在簿内、超出有效竞价范围(±2%)的存量订单隐藏
    void activateCage();

    // ==================== 快照（axob_snap.cpp）====================
    void onSnap(const AxsbeSnapStock& snap);
    void genSnap();
    void ensureSnap();          // [v2.7] 确保快照最新（需要时完整重建）
    void updateSnapStats();     // [v2.7] 增量更新统计字段（~5ns）
    AxsbeSnapStock genCallSnap(int showLevelNb = 5);   // [v2.7] 10->5 减少 fmtPx 调用
    AxsbeSnapStock genTradingSnap(bool isVolBreaking = false, int levelNb = 5);  // [v2.7] 10->5

    // ==================== 工具（axob_init.cpp）====================
    void useTimestamp(uint64_t transactTime);
    void setSnapFixParam(AxsbeSnapStock& snap);
    void setSnapTimestamp(AxsbeSnapStock& snap);
    void clipSnap(AxsbeSnapStock& snap);
    std::pair<std::map<int64_t,LevelNode>, std::map<int64_t,LevelNode>>
        getLevels(int levelNb);

    // [v2优化] orderMap 接口封装 — 保持外部接口兼容
    // 同时支持内存池和普通 new 两种分配策略
    ObOrder* getOrder(uint64_t seq) {
        auto it = orderMap.find(seq);
        return (it != orderMap.end()) ? it->second : nullptr;
    }
    const ObOrder* getOrder(uint64_t seq) const {
        auto it = orderMap.find(seq);
        return (it != orderMap.end()) ? it->second : nullptr;
    }
    void insertOrderMap(uint64_t seq, const ObOrder& order) {
#if USE_MEMORY_POOL
        ObOrder* p = orderPool_ ? orderPool_->alloc() : new ObOrder();
#else
        ObOrder* p = new ObOrder();
#endif
        *p = order;
        orderMap.emplace(seq, p);
    }
    void eraseOrderMap(uint64_t seq) {
        auto it = orderMap.find(seq);
        if (it != orderMap.end()) {
#if USE_MEMORY_POOL
            if (orderPool_) orderPool_->free(it->second);
            else delete it->second;
#else
            delete it->second;
#endif
            orderMap.erase(it);
        }
    }

    int orderMapSize()  const { return static_cast<int>(orderMap.size()); }
    int bidTreeSize()   const { return bidLevelBook.size(); }
    int askTreeSize()   const { return askLevelBook.size(); }
    int levelTreeSize() const { return bidTreeSize() + askTreeSize(); }

    std::string toString() const;
};

// =====================================================================
//  AXOB 级别静态断言
// =====================================================================
static_assert(std::is_trivially_copyable_v<LevelNode>,
              "LevelNode must be trivially copyable");
static_assert(sizeof(LevelNode) == 16,
              "LevelNode should be exactly 16B (int32 price + int64 qty)");
#if USE_MEMORY_POOL
static_assert(sizeof(ObOrder) >= sizeof(void*),
              "ObOrder must be >= sizeof(void*) for MemoryPool intrusive free list");
#endif
