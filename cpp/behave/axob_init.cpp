#include "axob.h"
#include <cstdio>
#include <algorithm>

// =====================================================================
//  AXOB 构造 + 工具函数
//  对应 Python axob.py: __init__ L377-469, _useTimestamp L1343,
//  _setSnapFixParam L1322, _setSnapTimestamp L1352, _clipSnap L1339,
//  _getLevels L1650, _fmtPrice_inter2snap L1628, __str__ L1777
// =====================================================================

AXOB::AXOB(int securityID, SecurityIDSource src, InstrumentType type)
    : SecurityID(securityID), secSrc(src), instType(type)
{
    // 判断是否创业板（300xxx）=> CYB 价格笼子
    if (src == SecurityIDSource_SZSE && securityID >= 300000 && securityID <= 309999) {
        cageType = CageType::CYB;
    } else {
        cageType = CageType::NONE;
    }
}

AXOB::~AXOB() {
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
static int32_t fmtPrice(int32_t price, InstrumentType instType, SecurityIDSource src) {
    if (src == SecurityIDSource_SZSE) {
        if (instType == InstrumentType::STOCK)
            return price * (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_STOCK_PRECISION);
        if (instType == InstrumentType::FUND)
            return price * (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_FUND_PRECISION);
    }
    return price;
}

// ---- 取N档买卖盘 ----
std::pair<std::map<int32_t,LevelNode>, std::map<int32_t,LevelNode>>
AXOB::getLevels(int levelNb) {
    std::map<int32_t,LevelNode> snapAsk, snapBid;

    // 卖方：从小到大
    int lv = 0;
    int32_t p = askMinPrice;
    int32_t q = askMinQty;
    for (int i = 0; i < levelNb; i++) {
        if (q != 0) {
            snapAsk[i] = LevelNode(fmtPrice(p, instType, secSrc), q);
            // 找下一个更高的卖方档位
            q = 0;
            auto it = askLevelTree.upper_bound(p);
            if (it != askLevelTree.end()) {
                p = it->first;
                q = it->second.qty;
            }
        } else {
            snapAsk[i] = LevelNode(0, 0);
        }
    }

    // 买方：从大到小
    p = bidMaxPrice;
    q = bidMaxQty;
    for (int i = 0; i < levelNb; i++) {
        if (q != 0) {
            snapBid[i] = LevelNode(fmtPrice(p, instType, secSrc), q);
            q = 0;
            auto it = bidLevelTree.lower_bound(p);
            if (it != bidLevelTree.begin()) {
                --it;
                p = it->first;
                q = it->second.qty;
            }
        } else {
            snapBid[i] = LevelNode(0, 0);
        }
    }

    return {snapAsk, snapBid};
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
