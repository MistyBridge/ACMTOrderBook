#pragma once
#include "axsbe_base.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

// =====================================================================
//  逐笔成交消息（对应 Python tool/axsbe_exe.py）
//  深交所 MsgType=191, 64字节
// =====================================================================

struct AxsbeExe {
    SecurityIDSource secSrc = SecurityIDSource_NULL;
    int      securityID      = 0;
    uint16_t ChannelNo       = 0;
    uint64_t ApplSeqNum      = 0;
    uint64_t BidApplSeqNum   = 0;
    uint64_t OfferApplSeqNum = 0;
    int64_t  LastPx          = 0;    // 原始精度
    int64_t  LastQty         = 0;
    uint8_t  ExecType        = 0;    // 'F'=成交, '4'=撤单
    uint64_t TransactTime    = 0;

    // ---- 从 Key=Value 字典加载 ----
#ifdef __GNUC__
    __attribute__((noinline))
#endif
    void loadDict(const std::unordered_map<std::string, int64_t>& dict) {
        secSrc         = static_cast<SecurityIDSource>(dict.at("SecurityIDSource"));
        securityID     = static_cast<int>(dict.at("SecurityID"));
        ChannelNo      = static_cast<uint16_t>(dict.at("ChannelNo"));
        ApplSeqNum     = static_cast<uint64_t>(dict.at("ApplSeqNum"));
        BidApplSeqNum  = static_cast<uint64_t>(dict.at("BidApplSeqNum"));
        OfferApplSeqNum= static_cast<uint64_t>(dict.at("OfferApplSeqNum"));
        LastPx         = dict.at("LastPx");
        LastQty        = dict.at("LastQty");
        ExecType       = static_cast<uint8_t>(dict.at("ExecType"));
        TransactTime   = static_cast<uint64_t>(dict.at("TransactTime"));
    }

    // ---- 辅助方法 ----
    bool isTrade()  const { return ExecType == 'F'; }
    bool isCancel() const { return ExecType == '4'; }

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

    uint64_t HHMMSSms() const {
        if (secSrc == SecurityIDSource_SZSE)
            return TransactTime % 1000000000ULL;
        return TransactTime;
    }

    std::string toString() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "%06d Seq=%llu Bid=%llu Offer=%llu Px=%lld Qty=%lld Type=%c @%llu",
            securityID, (unsigned long long)ApplSeqNum,
            (unsigned long long)BidApplSeqNum, (unsigned long long)OfferApplSeqNum,
            (long long)LastPx, (long long)LastQty,
            ExecType, (unsigned long long)TransactTime);
        return buf;
    }
};
