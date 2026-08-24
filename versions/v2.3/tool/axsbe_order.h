#pragma once
#include "axsbe_base.h"
#include "field_parser.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

// =====================================================================
//  逐笔委托消息（对应 Python tool/axsbe_order.py）
//  深交所 MsgType=192, 48字节
// =====================================================================

struct AxsbeOrder {
    SecurityIDSource secSrc = SecurityIDSource_NULL;
    int      securityID = 0;
    uint16_t ChannelNo  = 0;
    uint64_t ApplSeqNum = 0;
    int64_t  Price      = 0;     // 原始精度（深圳4位小数）
    int64_t  OrderQty   = 0;     // 原始精度（深圳2位小数）
    uint8_t  Side       = 0;     // '1'=买入, '2'=卖出
    uint8_t  OrdType    = 0;     // '1'=市价, '2'=限价, 'U'=本方最优
    uint64_t TransactTime = 0;

    // ---- 从 Key=Value 字典加载 ----
#ifdef __GNUC__
    __attribute__((noinline))
#endif
    void loadDict(const std::unordered_map<std::string, int64_t>& dict) {
        secSrc      = static_cast<SecurityIDSource>(dict.at("SecurityIDSource"));
        securityID  = static_cast<int>(dict.at("SecurityID"));
        ChannelNo   = static_cast<uint16_t>(dict.at("ChannelNo"));
        ApplSeqNum  = static_cast<uint64_t>(dict.at("ApplSeqNum"));

        if (secSrc == SecurityIDSource_SZSE) {
            Price       = dict.at("Price");
            OrderQty    = dict.at("OrderQty");
            Side        = static_cast<uint8_t>(dict.at("Side"));
            TransactTime = static_cast<uint64_t>(dict.at("TransactTime"));
            OrdType     = static_cast<uint8_t>(dict.at("OrdType"));
        }
    }

    // [v2.3] 直接从行字符串解析，跳过 dict 创建
    // 性能对比：loadDict() ~250ns vs loadFromLine() ~180ns
    void loadFromLine(const char* line) {
        int64_t value;

        // 注意：SecurityID 是 SecurityIDSource 的后缀，必须精确匹配
        // 使用 strstr("SecurityIDSource=") 来查找
        const char* srcPos = strstr(line, "SecurityIDSource=");
        if (srcPos) {
            char* endPtr = nullptr;
            secSrc = static_cast<SecurityIDSource>(strtoll(srcPos + 17, &endPtr, 10));
        }

        // 查找独立的 "SecurityID="（不是 "SecurityIDSource=" 的一部分）
        // 策略：查找 "SecurityID="，然后检查它前面不是 "Source"
        const char* idPos = strstr(line, "SecurityID=");
        if (idPos) {
            // 检查前面是否有 "Source"（表示这是 SecurityIDSource 的一部分）
            bool isSecurityIDSource = (idPos > line + 6) && (strncmp(idPos - 6, "Source", 6) == 0);
            if (!isSecurityIDSource) {
                char* endPtr = nullptr;
                securityID = static_cast<int>(strtoll(idPos + 11, &endPtr, 10));
            }
        }

        // 解析其他字段
        if (extractField(line, "ChannelNo", value))
            ChannelNo = static_cast<uint16_t>(value);

        if (extractField(line, "ApplSeqNum", value))
            ApplSeqNum = static_cast<uint64_t>(value);

        // 解析 Price, OrderQty, Side, TransactTime, OrdType
        // 注意：这些字段只在 SZSE 消息中存在
        if (secSrc == SecurityIDSource_SZSE) {
            if (extractField(line, "Price", value))
                Price = value;

            if (extractField(line, "OrderQty", value))
                OrderQty = static_cast<int32_t>(value);

            if (extractField(line, "Side", value))
                Side = static_cast<uint8_t>(value);

            if (extractField(line, "TransactTime", value))
                TransactTime = static_cast<uint64_t>(value);

            if (extractField(line, "OrdType", value))
                OrdType = static_cast<uint8_t>(value);
        }
    }

    // ---- 辅助方法 ----
    bool isBuy()  const { return Side == '1'; }
    bool isSell() const { return Side == '2'; }

    bool isMarket()      const { return OrdType == '1'; }
    bool isLimit()       const { return OrdType == '2' || OrdType == 'A'; }  // '2'=SZ限价, 'A'=SH新增(限价)
    bool isSideOptimal() const { return OrdType == 'U'; }

    // 根据时戳推算交易阶段（对应 axsbe_base.py TradingPhaseMarket）
    TPM tradingPhaseMarket() const {
        uint64_t t = HHMMSSms();
        if (t < 91500000)  return TPM::Starting;
        if (t < 92500000)  return TPM::OpenCall;
        if (t < 93000000)  return TPM::PreTradingBreaking;
        if (t < 113000000) return TPM::AMTrading;
        if (t < 130000000) return TPM::Breaking;
        if (t < 145700000) return TPM::PMTrading;
        if (t < 150000000) return TPM::CloseCall;
        return TPM::Ending;
    }

    // 日内时戳（精度ms）
    uint64_t HHMMSSms() const {
        if (secSrc == SecurityIDSource_SZSE)
            return TransactTime % 1000000000ULL;
        return TransactTime;
    }

    std::string toString() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "%06d Seq=%llu Px=%lld Qty=%lld Side=%c Type=%c @%llu",
            securityID, (unsigned long long)ApplSeqNum,
            (long long)Price, (long long)OrderQty,
            Side, OrdType, (unsigned long long)TransactTime);
        return buf;
    }
};
