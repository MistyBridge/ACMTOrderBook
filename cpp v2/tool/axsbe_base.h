#pragma once
#include <cstdint>
#include <cstring>   // memcmp, strncmp [v2.6]
#include <cstdlib>   // strtoll [v2.6]
#include <string>

// ==================== 交易所代码 ====================
enum SecurityIDSource : uint8_t {
    SecurityIDSource_NULL = 0,
    SecurityIDSource_SSE  = 101,
    SecurityIDSource_SZSE = 102,
};

// =====================================================================
//  消息基类模板
//
//  [v2.3] 代码模板化重构
//  提取通用的 SecurityIDSource/SecurityID 解析逻辑，减少代码重复
//
//  使用方式：
//    class AxsbeOrder : public AxsbeMessageBase<AxsbeOrder> {
//    public:
//        // 其他字段
//        void loadFromLineImpl(const char* line) {
//            // 只需要解析特定字段
//        }
//    };
// =====================================================================

template<typename Derived>
class AxsbeMessageBase {
public:
    SecurityIDSource secSrc;
    int securityID;

    // 构造函数初始化
    AxsbeMessageBase() : secSrc(SecurityIDSource_NULL), securityID(0) {}
    AxsbeMessageBase(SecurityIDSource src, int id) : secSrc(src), securityID(id) {}

    // 通用的 loadFromLine 实现
    // 调用派生类的 loadFromLineImpl() 实现特定字段解析
    void loadFromLine(const char* line) {
        // 调用通用解析（SecurityIDSource/SecurityID）
        loadFromLineCommon(line);
        // 调用派生类特定解析
        static_cast<Derived*>(this)->loadFromLineImpl(line);
    }

    // [v2.6] 零分配版本 — 接受 (ptr, end) 对，避免 std::string 分配
    void loadFromLine(const char* lineStart, const char* lineEnd) {
        loadFromLineCommon(lineStart, lineEnd);
        static_cast<Derived*>(this)->loadFromLineImpl(lineStart, lineEnd);
    }

protected:
    // [v2.6] 零分配版通用解析（内联，避免循环依赖 field_parser.h）
    // 手动整数解析，不修改源内存（mmap 只读安全）
    void loadFromLineCommon(const char* lineStart, const char* lineEnd) {
        // SecurityIDSource 解析
        const char* srcKey = "SecurityIDSource=";
        const size_t srcKeyLen = 17;
        for (const char* p = lineStart; p + srcKeyLen <= lineEnd; ++p) {
            if (*p == 'S' && memcmp(p, srcKey, srcKeyLen) == 0) {
                const char* valStart = p + srcKeyLen;
                if (valStart < lineEnd) {
                    int64_t val = 0;
                    bool neg = false;
                    const char* vp = valStart;
                    if (*vp == '-') { neg = true; ++vp; }
                    while (vp < lineEnd && *vp >= '0' && *vp <= '9') {
                        val = val * 10 + (*vp - '0'); ++vp;
                    }
                    if (vp > valStart + (neg ? 1 : 0))
                        secSrc = static_cast<SecurityIDSource>(neg ? -val : val);
                }
                break;
            }
        }
        // SecurityID 解析（排除 SecurityIDSource）
        const char* idKey = "SecurityID=";
        const size_t idKeyLen = 11;
        for (const char* p = lineStart; p + idKeyLen <= lineEnd; ++p) {
            if (*p == 'S' && memcmp(p, idKey, idKeyLen) == 0) {
                bool isSource = (p >= lineStart + 6) && (memcmp(p - 6, "Source", 6) == 0);
                if (!isSource) {
                    const char* valStart = p + idKeyLen;
                    if (valStart < lineEnd) {
                        int64_t val = 0;
                        const char* vp = valStart;
                        while (vp < lineEnd && *vp >= '0' && *vp <= '9') {
                            val = val * 10 + (*vp - '0'); ++vp;
                        }
                        if (vp > valStart)
                            securityID = static_cast<int>(val);
                    }
                    break;
                }
            }
        }
    }

    // 通用的 SecurityIDSource/SecurityID 解析
    void loadFromLineCommon(const char* line) {
        // SecurityIDSource 解析
        const char* srcPos = strstr(line, "SecurityIDSource=");
        if (srcPos) {
            char* endPtr = nullptr;
            int64_t value = strtoll(srcPos + 17, &endPtr, 10);
            if (endPtr != srcPos + 17) {
                secSrc = static_cast<SecurityIDSource>(value);
            }
        }

        // SecurityID 解析（需要排除 SecurityIDSource）
        const char* idPos = strstr(line, "SecurityID=");
        if (idPos) {
            // 确保不是 SecurityIDSource 的一部分
            bool isSource = (idPos > line + 6) &&
                           (strncmp(idPos - 6, "Source", 6) == 0);
            if (!isSource) {
                char* endPtr = nullptr;
                int64_t value = strtoll(idPos + 11, &endPtr, 10);
                if (endPtr != idPos + 11) {
                    securityID = static_cast<int>(value);
                }
            }
        }
    }

    // 派生类实现此方法，用于解析特定字段
    // 默认空实现
    void loadFromLineImpl(const char* line) {
        // 派生类可以覆盖此方法
    }
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
// 快照价格精度 ×10^4（与 py 引擎及 AX-SBE 历史文件一致；A股价格范围 int32 足够，无需 ×10^6）
constexpr int PRICE_SZSE_SNAP_PRECISION  = 10000;
constexpr int PRICE_SSE_PRECISION        = 1000;

// ==================== 内部计算精度 (统一定点 ×10^5) ====================
// 价格/数量/金额一律 ×10^5 (最小可表示单位 1e-5), 不再按交易所/品种分叉。
// 选 1e-5 的理由: ①深交所数量原生 2 位小数、上交所 3 位小数, 取更细者仍有余量;
//   ②基金/可转债价格 3 位小数无需特例; ③碎股 (境外市场可至 1e-5 股) 可直接表示。
// 字段位宽: 价格/数量/金额均为 int64 — ×10^5 下 int32 上限仅 21474.83, 必然越界。
constexpr int64_t INTER_PRECISION = 100000;

// 内部精度别名 (语义清晰, 值同为 INTER_PRECISION)
constexpr int64_t PRICE_INTER_PRECISION = INTER_PRECISION;
constexpr int64_t QTY_INTER_PRECISION   = INTER_PRECISION;
constexpr int64_t AMT_INTER_PRECISION   = INTER_PRECISION;

// ==================== 精度转换因子 (原始 → 内部, 乘法制) ====================
// 原始精度均低于内部精度, 故一律为乘 (无截断, 无精度损失)。
// 反向 (内部 → 原始) 一律为除, 见 fmtPriceInter2Snap / qtyInter2Snap。
constexpr int64_t SZSE_PRICE_MUL = INTER_PRECISION / PRICE_SZSE_INCR_PRECISION;  // ×10
constexpr int64_t SSE_PRICE_MUL  = INTER_PRECISION / PRICE_SSE_PRECISION;        // ×100

// ==================== 价格溢出 ====================
constexpr int64_t ORDER_PRICE_OVERFLOW = 0x7FFFFFFF;   // 原始价格越界标记 (原始精度域)
constexpr int64_t PRICE_MAXIMUM = INT64_MAX;           // 本地越界上限 (内部精度域)

// ==================== 创业板委托市值上限倍率 ====================
constexpr int CYB_ORDER_ENVALUE_MAX_RATE = 9;

// ==================== 其他精度常量 ====================
constexpr int PRICE_SZSE_SNAP_PRECLOSE_PRECISION = 10000;

// 原始快照成交额精度 (交易所口径, 仅用于喂入/回吐换算; 内部一律 AMT_INTER_PRECISION)
constexpr int64_t TOTALVALUETRADE_SZSE_PRECISION = 10000;
constexpr int64_t TOTALVALUETRADE_SSE_PRECISION  = 100000;

// 原始数量精度 (交易所口径): 深 2 位小数, 沪 3 位小数
constexpr int64_t QTY_SZSE_PRECISION = 100;
constexpr int64_t QTY_SSE_PRECISION  = 1000;

// 数量换算因子 (原始 → 内部 ×10^5, 乘法制)
constexpr int64_t SZSE_QTY_MUL = INTER_PRECISION / QTY_SZSE_PRECISION;   // ×1000
constexpr int64_t SSE_QTY_MUL  = INTER_PRECISION / QTY_SSE_PRECISION;    // ×100

// 成交额换算因子 (内部 ×10^5 → 原始快照精度, 除法制)
constexpr int64_t SZSE_AMT_DIV = AMT_INTER_PRECISION / TOTALVALUETRADE_SZSE_PRECISION;  // ÷10
constexpr int64_t SSE_AMT_DIV  = AMT_INTER_PRECISION / TOTALVALUETRADE_SSE_PRECISION;   // ÷1

// ---- 内部 ↔ 原始 换算辅助 (数量/金额; 价格见 ob_types.h::fmtPriceInter2Snap) ----
inline int64_t qtySnap2Inter(int64_t rawQty, SecurityIDSource src) {
    return (src == SecurityIDSource_SZSE) ? rawQty * SZSE_QTY_MUL
         : (src == SecurityIDSource_SSE)  ? rawQty * SSE_QTY_MUL
                                          : rawQty;
}
inline int64_t qtyInter2Snap(int64_t qty, SecurityIDSource src) {
    return (src == SecurityIDSource_SZSE) ? qty / SZSE_QTY_MUL
         : (src == SecurityIDSource_SSE)  ? qty / SSE_QTY_MUL
                                          : qty;
}
inline int64_t amtInter2Snap(int64_t amt, SecurityIDSource src) {
    return (src == SecurityIDSource_SZSE) ? amt / SZSE_AMT_DIV
         : (src == SecurityIDSource_SSE)  ? amt / SSE_AMT_DIV
                                          : amt;
}
