#pragma once
#include <cstdint>
#include <string>

// ==================== 交易所代码 ====================
enum SecurityIDSource : uint8_t {
    SecurityIDSource_NULL = 0,
    SecurityIDSource_SSE  = 101,
    SecurityIDSource_SZSE = 102,
};

// ==================== 消息类型 ====================
enum MsgType : uint8_t {
    MsgType_exe             = 191,
    MsgType_order           = 192,
    MsgType_snap            = 111,
    MsgType_exe_sse_bond    = 84,
    MsgType_order_sse_bond_add   = 81,
    MsgType_order_sse_bond_del   = 82,
    MsgType_snap_sse_bond        = 83,
};

inline bool isExeType(uint8_t t)  { return t==MsgType_exe || t==MsgType_exe_sse_bond; }
inline bool isOrdType(uint8_t t)  { return t==MsgType_order || t==MsgType_order_sse_bond_add || t==MsgType_order_sse_bond_del; }
inline bool isSnapType(uint8_t t) { return t==MsgType_snap || t==MsgType_snap_sse_bond; }

// ==================== 证券类型 ====================
enum class InstrumentType : uint8_t {
    STOCK  = 0,
    FUND   = 1,
    KZZ    = 2,
    OPTION = 3,
    BOND   = 4,
    NHG    = 5,
    UNKNOWN = 0xFF,
};

// ==================== 市场交易阶段 (TPM) ====================
enum class TPM : int8_t {
    Starting             = 0,
    OpenCall             = 1,
    PreTradingBreaking   = 2,
    AMTrading            = 3,
    Breaking             = 4,
    PMTrading            = 5,
    CloseCall            = 6,
    AfterCloseTrading    = 7,
    VolatilityBreaking   = 8,
    Ending               = 9,
    HangingUp            = 10,
    Fusing               = 11,
    Unknown              = -1,
};

inline const char* tpm_str(TPM t) {
    switch (t) {
        case TPM::Starting:           return "Starting";
        case TPM::OpenCall:           return "OpenCall";
        case TPM::PreTradingBreaking: return "PreTradingBreaking";
        case TPM::AMTrading:          return "AMTrading";
        case TPM::Breaking:           return "Breaking";
        case TPM::PMTrading:          return "PMTrading";
        case TPM::CloseCall:          return "CloseCall";
        case TPM::AfterCloseTrading:  return "AfterCloseTrading";
        case TPM::VolatilityBreaking: return "VolatilityBreaking";
        case TPM::Ending:             return "Ending";
        case TPM::HangingUp:          return "HangingUp";
        case TPM::Fusing:             return "Fusing";
        default:                      return "Unknown";
    }
}

// ==================== 标的交易状态 (TPI) ====================
enum class TPI : int8_t {
    Normal  = 0,
    NoTrade = 1,
    Unknown = -1,
};

inline const char* tpi_str(TPI t) {
    switch (t) {
        case TPI::Normal:  return "Normal";
        case TPI::NoTrade: return "NoTrade";
        default:           return "Unknown";
    }
}

// ==================== 买卖方向 ====================
enum class Side : int8_t {
    BID     = 0,
    ASK     = 1,
    UNKNOWN = -1,
};

inline const char* side_str(Side s) {
    switch (s) {
        case Side::BID: return "BID";
        case Side::ASK: return "ASK";
        default:        return "UNKNOWN";
    }
}

// ==================== 委托类型 ====================
enum class OrdType : int8_t {
    LIMIT   = 0,
    MARKET  = 1,
    SIDE    = 2,
    UNKNOWN = -1,
};

// ==================== 成交类型 ====================
enum class ExecType : uint8_t {
    TRADE  = 'F',
    CANCEL = '4',
};

// ==================== 原始数据精度 ====================
constexpr int PRICE_SZSE_INCR_PRECISION  = 10000;
constexpr int PRICE_SZSE_SNAP_PRECISION  = 1000000;
constexpr int PRICE_SSE_PRECISION        = 1000;

// ==================== 内部计算精度 ====================
constexpr int PRICE_INTER_STOCK_PRECISION = 100;
constexpr int PRICE_INTER_FUND_PRECISION  = 1000;
constexpr int QTY_INTER_SZSE_PRECISION   = 100;
constexpr int QTY_INTER_SSE_PRECISION    = 1000;

// ==================== 内部计算精度（KZZ）====================
constexpr int PRICE_INTER_KZZ_PRECISION = 1000;  // 可转债：3位小数

// ==================== 精度转换因子 ====================
constexpr int SZSE_STOCK_PRICE_RD = PRICE_SZSE_INCR_PRECISION / PRICE_INTER_STOCK_PRECISION;
constexpr int SZSE_FUND_PRICE_RD  = PRICE_SZSE_INCR_PRECISION / PRICE_INTER_FUND_PRECISION;
constexpr int SZSE_KZZ_PRICE_RD   = PRICE_SZSE_INCR_PRECISION / PRICE_INTER_KZZ_PRECISION;
constexpr int SSE_STOCK_PRICE_RD  = PRICE_SSE_PRECISION       / PRICE_INTER_STOCK_PRECISION;

// ==================== 价格溢出 ====================
constexpr int64_t ORDER_PRICE_OVERFLOW = 0x7FFFFFFF;  // 原始价格越界标记
constexpr int32_t PRICE_MAXIMUM = 0x7FFFFFFF;          // 本地越界上限

// ==================== 创业板委托市值上限倍率 ====================
constexpr int CYB_ORDER_ENVALUE_MAX_RATE = 9;

// ==================== 其他精度常量 ====================
constexpr int PRICE_SZSE_SNAP_PRECLOSE_PRECISION = 10000;
constexpr int TOTALVALUETRADE_SZSE_PRECISION     = 10000;
constexpr int TOTALVALUETRADE_SSE_PRECISION      = 100000;
