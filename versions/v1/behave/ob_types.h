#pragma once
#include <cstdint>
#include "../tool/axsbe_base.h"
#include "../tool/axsbe_order.h"
#include "../tool/axsbe_exe.h"

// =====================================================================
//  内部数据结构（对应 Python axob.py 中的 ob_order/ob_exec/
//               ob_cancel/level_node）
//  所有价格/数量统一使用内部精度（股票2位小数）
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
struct ObOrder {
    uint64_t applSeqNum;
    int32_t  price;
    int32_t  qty;
    Side     side;
    OrdType  type;
    bool     traded = false;
    uint64_t TransactTime = 0;

    ObOrder() : applSeqNum(0), price(0), qty(0), side(Side::UNKNOWN), type(OrdType::UNKNOWN) {}

    ObOrder(const AxsbeOrder& raw, InstrumentType instType)
        : applSeqNum(raw.ApplSeqNum), qty(static_cast<int32_t>(raw.OrderQty)),
          TransactTime(raw.TransactTime)
    {
        // 价格溢出检查
        if (raw.Price == ORDER_PRICE_OVERFLOW) {
            price = PRICE_MAXIMUM;
        } else {
            // 精度转换
            if (raw.secSrc == SecurityIDSource_SZSE) {
                if (instType == InstrumentType::STOCK)
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
struct ObExec {
    int32_t  LastPx;
    int32_t  LastQty;
    uint64_t BidApplSeqNum;
    uint64_t OfferApplSeqNum;
    uint64_t TransactTime;
    TPM      tradingPhaseMarket;

    ObExec(const AxsbeExe& raw, InstrumentType instType)
        : LastQty(static_cast<int32_t>(raw.LastQty)),
          BidApplSeqNum(raw.BidApplSeqNum),
          OfferApplSeqNum(raw.OfferApplSeqNum),
          TransactTime(raw.TransactTime)
    {
        if (raw.secSrc == SecurityIDSource_SZSE) {
            if (instType == InstrumentType::STOCK)
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
struct ObCancel {
    uint64_t applSeqNum;
    int32_t  qty;
    int32_t  price;
    Side     side;
    uint64_t TransactTime;
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
