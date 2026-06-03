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

struct AxsbeOrder : public AxsbeMessageBase<AxsbeOrder> {
    // secSrc 和 securityID 已在基类中定义

    uint16_t ChannelNo  = 0;
    uint64_t ApplSeqNum = 0;
    int64_t  Price      = 0;     // 原始精度（深圳4位小数）
    int64_t  OrderQty   = 0;     // 原始精度（深圳2位小数）
    uint8_t  Side       = 0;     // '1'=买入, '2'=卖出
    uint8_t  OrdType    = 0;     // '1'=市价, '2'=限价, 'U'=本方最优
    uint64_t TransactTime = 0;

    // ---- 从 Key=Value 字典加载 ----
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
    void loadFromLineImpl(const char* line) {
        int64_t value;
        if (extractField(line, "ChannelNo", value))
            ChannelNo = static_cast<uint16_t>(value);
        if (extractField(line, "ApplSeqNum", value))
            ApplSeqNum = static_cast<uint64_t>(value);
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

    // [v2.6] 零分配版本
    // [v2.8] 跳过 ChannelNo 解析（AXOB 不使用 Order 的 ChannelNo）
    // [v2.8] 前向 strstr 优化：ORDER 字段顺序固定，从上次位置继续搜索
    void loadFromLineImpl(const char* s, const char* e) {
        int64_t value;
        FieldParser parser(s, e);
        if (parser.find("ApplSeqNum", value))
            ApplSeqNum = static_cast<uint64_t>(value);
        if (secSrc == SecurityIDSource_SZSE) {
            if (parser.find("Price", value))        Price = value;
            if (parser.find("OrderQty", value))     OrderQty = static_cast<int32_t>(value);
            if (parser.find("Side", value))          Side = static_cast<uint8_t>(value);
            if (parser.find("OrdType", value))      OrdType = static_cast<uint8_t>(value);
            if (parser.find("TransactTime", value)) TransactTime = static_cast<uint64_t>(value);
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
