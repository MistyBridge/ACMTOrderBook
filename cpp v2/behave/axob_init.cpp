#include "axob.h"
#include <cstdio>
#include <algorithm>

// =====================================================================
//  AXOB 构造 + 工具函数
//  对应 Python axob.py: __init__ L377-469, _useTimestamp L1343,
//  _setSnapFixParam L1322, _setSnapTimestamp L1352, _clipSnap L1339,
//  _getLevels L1650, _fmtPrice_inter2snap L1628, __str__ L1777
// =====================================================================

// [v2优化] 构造函数：支持外部传入 MemoryPool，或自行创建
#if USE_MEMORY_POOL
AXOB::AXOB(int securityID, SecurityIDSource src, InstrumentType type,
           axob::core::MemoryPool<ObOrder>* pool)
    : SecurityID(securityID), secSrc(src), instType(type)
{
    // 内存池初始化：外部传入则复用，否则自建
    if (pool) {
        orderPool_ = pool;
        ownPool_ = false;
    } else {
        orderPool_ = new axob::core::MemoryPool<ObOrder>(4096);
        ownPool_ = true;
    }

    // 市场子类型自动识别
    mktSubType = marketSubType(src, securityID);

    // 创业板价格笼子
    cageType = (mktSubType == MarketSubType::SZSE_STK_GEM)
               ? CageType::CYB : CageType::NONE;
}
#else
AXOB::AXOB(int securityID, SecurityIDSource src, InstrumentType type)
    : SecurityID(securityID), secSrc(src), instType(type)
{
    mktSubType = marketSubType(src, securityID);
    cageType = (mktSubType == MarketSubType::SZSE_STK_GEM)
               ? CageType::CYB : CageType::NONE;
}
#endif

// [v2优化] 析构函数：释放 orderMap 中所有堆分配的 ObOrder
AXOB::~AXOB() {
    // 释放 orderMap 中的 ObOrder*
    for (auto& [seq, ptr] : orderMap) {
#if USE_MEMORY_POOL
        if (orderPool_) orderPool_->free(ptr);
        else delete ptr;
#else
        delete ptr;
#endif
    }
    orderMap.clear();

#if USE_MEMORY_POOL
    // 如果自行创建了内存池，释放它
    if (ownPool_ && orderPool_) {
        delete orderPool_;
        orderPool_ = nullptr;
    }
#endif

    delete lastSnap;
}

// ---- 时戳转内部精度 ----
void AXOB::useTimestamp(uint64_t transactTime) {
    if (secSrc == SecurityIDSource_SZSE) {
        currentIncTick = (transactTime / SZSE_TICK_MS_TAIL) %
                         (SZSE_TICK_CUT / SZSE_TICK_MS_TAIL);
    } else {
        currentIncTick = transactTime;
    }
    if (currentIncTick >= (1ULL << TIMESTAMP_BIT_SIZE)) {
        fprintf(stderr, "%06d TransactTime=%llu ovf!\n", SecurityID,
                (unsigned long long)transactTime);
    }
}

// ---- 快照固定参数 ----
void AXOB::setSnapFixParam(AxsbeSnapStock& snap) {
    snap.securityID = SecurityID;
    snap.ChannelNo  = mktInfo.ChannelNo;
    snap.UpLimitPx  = mktInfo.UpLimitPx;
    snap.DnLimitPx  = mktInfo.DnLimitPx;

    if (secSrc == SecurityIDSource_SZSE) {
        if (instType == InstrumentType::STOCK)
            snap.PrevClosePx = mktInfo.PrevClosePx *
                (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_STOCK_PRECISION);
        else if (instType == InstrumentType::FUND)
            snap.PrevClosePx = mktInfo.PrevClosePx *
                (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_FUND_PRECISION);
        else
            snap.PrevClosePx = mktInfo.PrevClosePx;
    }
}

// ---- 时戳写入快照 ----
void AXOB::setSnapTimestamp(AxsbeSnapStock& snap) {
    if (secSrc == SecurityIDSource_SZSE) {
        snap.TransactTime = mktInfo.YYMMDD * SZSE_TICK_CUT +
                           (currentIncTick * SZSE_TICK_MS_TAIL);
    } else {
        snap.TransactTime = currentIncTick / 100;
    }
}

// ---- 大数钳位 ----
void AXOB::clipSnap(AxsbeSnapStock& snap) {
    snap.AskWeightPx = clipInt32(static_cast<int64_t>(snap.AskWeightPx));
}

// ---- 内部精度转快照精度 ----
[[maybe_unused]] static int32_t fmtPrice(int32_t price, InstrumentType instType, SecurityIDSource src) {
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

// ---- 取N档买卖盘（返回内部精度，与 v1 一致）----
std::pair<std::map<int32_t,LevelNode>, std::map<int32_t,LevelNode>>
AXOB::getLevels(int levelNb) {
    std::map<int32_t,LevelNode> askResult, bidResult;

    // 卖方：从小到大（CompactLevelBook 已按升序排列）
    {
        int idx = 0;
        for (int i = 0; i < askLevelBook.count && idx < levelNb; i++) {
            askResult[idx] = askLevelBook.levels[i];
            idx++;
        }
        for (; idx < levelNb; idx++) askResult[idx] = LevelNode(0, 0);
    }

    // 买方：从大到小（CompactLevelBook 升序，反向遍历）
    {
        int idx = 0;
        for (int i = bidLevelBook.count - 1; i >= 0 && idx < levelNb; i--) {
            bidResult[idx] = bidLevelBook.levels[i];
            idx++;
        }
        for (; idx < levelNb; idx++) bidResult[idx] = LevelNode(0, 0);
    }

    return {askResult, bidResult};
}

// ---- 打印状态 ----
std::string AXOB::toString() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "%06d tick=%llu msgs=%d\n"
        "  orderMap=%d bidTree=%d askTree=%d\n"
        "  bidMax=%d*%d  askMin=%d*%d\n"
        "  LastPx=%d HighPx=%d LowPx=%d OpenPx=%d\n"
        "  NumTrades=%lld TVol=%lld TVal=%lld\n"
        "  tradingPhase=%s",
        SecurityID, (unsigned long long)currentIncTick, msgNb,
        orderMapSize(), bidTreeSize(), askTreeSize(),
        bidMaxPrice, bidMaxQty, askMinPrice, askMinQty,
        LastPx, HighPx, LowPx, OpenPx,
        (long long)NumTrades, (long long)TotalVolumeTrade, (long long)TotalValueTrade,
        tpm_str(tradingPhase));
    return buf;
}
