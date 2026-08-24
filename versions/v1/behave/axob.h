#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include "ob_types.h"
#include "../tool/axsbe_base.h"
#include "../tool/axsbe_order.h"
#include "../tool/axsbe_exe.h"
#include "../tool/axsbe_snap_stock.h"
#include "../tool/msg_util.h"

// =====================================================================
//  AXOB — 订单簿重建引擎（声明）
//
//  实现分布在多个 .cpp 文件中：
//    axob_init.cpp     构造、工具函数
//    axob_order.cpp    委托处理
//    axob_trade.cpp    成交/撤单处理
//    axob_cage.cpp     创业板价格笼子
//    axob_snap.cpp     快照生成与验证
// =====================================================================

class AXOB {
public:
    // ==================== 股票标识 ====================
    int              SecurityID;
    SecurityIDSource secSrc;
    InstrumentType   instType;

    // ==================== 核心数据结构 ====================
    std::unordered_map<uint64_t, ObOrder> orderMap;
    std::map<int32_t, LevelNode>          bidLevelTree;
    std::map<int32_t, LevelNode>          askLevelTree;

    // ==================== 市场统计 ====================
    int64_t NumTrades         = 0;
    int64_t TotalVolumeTrade  = 0;
    int64_t TotalValueTrade   = 0;
    int32_t LastPx            = 0;
    int32_t HighPx            = 0;
    int32_t LowPx             = 0;
    int32_t OpenPx            = 0;

    // ==================== 最优档缓存 ====================
    int32_t bidMaxPrice = 0, bidMaxQty = 0;
    int32_t askMinPrice = 0, askMinQty = 0;

    // ==================== 市场常量 ====================
    MarketInfo mktInfo;
    bool closePxReady = false;

    // ==================== 时戳 ====================
    uint64_t currentIncTick = 0;
    static constexpr uint64_t SZSE_TICK_CUT     = 1000000000ULL;
    static constexpr int      SZSE_TICK_MS_TAIL  = 10;
    static constexpr int      TIMESTAMP_BIT_SIZE = 28;  // 上交所精度1ms，最大28b

    // ==================== 加权统计 ====================
    int64_t BidWeightSize    = 0;
    int64_t BidWeightValue   = 0;
    int64_t AskWeightSize    = 0;
    int64_t AskWeightValue   = 0;
    int64_t AskWeightSizeEx  = 0;
    int64_t AskWeightValueEx = 0;

    // ==================== 缓存单 ====================
    std::unique_ptr<ObOrder> holdingOrder;
    int holdingNb = 0;

    // ==================== 交易阶段 ====================
    TPM tradingPhase = TPM::Starting;
    int volatilityBreakingEndTick = 0;

    // ==================== 市场子类型 ====================
    MarketSubType mktSubType = MarketSubType::SZSE_STK_MB;

    // ==================== 价格笼子 ====================
    CageType cageType = CageType::NONE;
    CageSide cageSide = CageSide::NONE;
    int32_t bidCageUpperExMinPrice = 0;
    int32_t bidCageUpperExMinQty   = 0;
    int32_t askCageLowerExMaxPrice = 0;
    int32_t askCageLowerExMaxQty   = 0;
    int32_t bidCageRefPx           = 0;
    int32_t askCageRefPx           = 0;
    bool    bidWaitingForCage      = false;
    bool    askWaitingForCage      = false;

    // ==================== 导出开关（FPGA验证用）====================
    bool exportLevelAccess = false;

    // ==================== 调试/测试 ====================
    int msgNb = 0;
    AxsbeSnapStock* lastSnap = nullptr;

    // ==================== 构造 ====================
    AXOB(int securityID, SecurityIDSource src, InstrumentType type);
    ~AXOB();

    // ==================== 消息入口（重载分发）====================
    void onMsg(const AxsbeOrder& msg);
    void onMsg(const AxsbeExe&   msg);
    void onMsg(const AxsbeSnapStock& msg);
    void onMsg(AXSignal signal);

    // ==================== 委托处理（axob_order.cpp）====================
    void onOrder(const AxsbeOrder& rawOrder);
    void onLimitOrder(ObOrder& order);
    void insertOrder(const ObOrder& order, bool outOfCage = false);

    // ==================== 成交/撤单（axob_trade.cpp）====================
    void onExec(const AxsbeExe& rawExec);
    void onTrade(const ObExec& exec);
    void onCancel(const ObCancel& cancel);
    void tradeLimit(Side side, int32_t qty, uint64_t applSeqNum);
    void levelDequeue(Side side, int32_t price, int32_t qty, uint64_t applSeqNum);

    // ==================== 价格笼子（axob_cage.cpp）====================
    void openCage();
    void enterCage();

    // ==================== 快照（axob_snap.cpp）====================
    void onSnap(const AxsbeSnapStock& snap);
    void genSnap();
    AxsbeSnapStock genCallSnap(int showLevelNb = 10);
    AxsbeSnapStock genTradingSnap(bool isVolBreaking = false, int levelNb = 10);

    // ==================== 工具（axob_init.cpp）====================
    void useTimestamp(uint64_t transactTime);
    void setSnapFixParam(AxsbeSnapStock& snap);
    void setSnapTimestamp(AxsbeSnapStock& snap);
    void clipSnap(AxsbeSnapStock& snap);
    std::pair<std::map<int32_t,LevelNode>, std::map<int32_t,LevelNode>>
        getLevels(int levelNb);

    int orderMapSize()  const { return static_cast<int>(orderMap.size()); }
    int bidTreeSize()   const { return static_cast<int>(bidLevelTree.size()); }
    int askTreeSize()   const { return static_cast<int>(askLevelTree.size()); }
    int levelTreeSize() const { return bidTreeSize() + askTreeSize(); }

    std::string toString() const;
};
