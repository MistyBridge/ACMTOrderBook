#pragma once
#include "axsbe_base.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

// =====================================================================
//  行情快照消息（对应 Python tool/axsbe_snap_stock.py）
//  深交所 MsgType=111, 352字节, 10档盘口
// =====================================================================

struct PriceLevel {
    int64_t Price = 0;   // 快照精度（深圳6位小数）
    int64_t Qty   = 0;

    PriceLevel() = default;
    PriceLevel(int64_t p, int64_t q) : Price(p), Qty(q) {}

    bool operator==(const PriceLevel& o) const {
        return Price == o.Price && Qty == o.Qty;
    }
    bool operator!=(const PriceLevel& o) const { return !(*this == o); }
};

struct AxsbeSnapStock {
    SecurityIDSource secSrc = SecurityIDSource_NULL;
    int      securityID      = 0;
    uint16_t ChannelNo       = 0;
    uint8_t  TradingPhaseCode= 0;
    int64_t  NumTrades       = 0;
    int64_t  TotalVolumeTrade= 0;
    int64_t  TotalValueTrade = 0;
    int64_t  PrevClosePx     = 0;
    int64_t  LastPx          = 0;
    int64_t  OpenPx          = 0;
    int64_t  HighPx          = 0;
    int64_t  LowPx           = 0;
    int64_t  BidWeightPx     = 0;
    int64_t  BidWeightSize   = 0;
    int64_t  AskWeightPx     = 0;
    int64_t  AskWeightSize   = 0;
    int64_t  UpLimitPx       = 0;
    int64_t  DnLimitPx       = 0;
    PriceLevel bid[10];
    PriceLevel ask[10];
    uint64_t TransactTime    = 0;

    // 调试用
    int  _seq    = -1;
    char _source[16] = "MD";

    // ---- 从 Key=Value 字典加载 ----
    void loadDict(const std::unordered_map<std::string, int64_t>& dict) {
        secSrc      = static_cast<SecurityIDSource>(dict.at("SecurityIDSource"));
        securityID  = static_cast<int>(dict.at("SecurityID"));
        ChannelNo   = static_cast<uint16_t>(dict.at("ChannelNo"));

        if (secSrc == SecurityIDSource_SZSE) {
            TradingPhaseCode = static_cast<uint8_t>(dict.at("TradingPhase"));
            NumTrades        = dict.at("NumTrades");
            TotalVolumeTrade = dict.at("TotalVolumeTrade");
            TotalValueTrade  = dict.at("TotalValueTrade");
            PrevClosePx      = dict.at("PrevClosePx");
            LastPx           = dict.at("LastPx");
            OpenPx           = dict.at("OpenPx");
            HighPx           = dict.at("HighPx");
            LowPx            = dict.at("LowPx");
            BidWeightPx      = dict.at("BidWeightPx");
            BidWeightSize    = dict.at("BidWeightSize");
            AskWeightPx      = dict.at("AskWeightPx");
            AskWeightSize    = dict.at("AskWeightSize");
            UpLimitPx        = dict.at("UpLimitPx");
            DnLimitPx        = dict.at("DnLimitPx");
            TransactTime     = static_cast<uint64_t>(dict.at("TransactTime"));

            for (int i = 0; i < 10; i++) {
                char keyP[32], keyQ[32];
                snprintf(keyP, sizeof(keyP), "BidLevel[%d].Price", i);
                snprintf(keyQ, sizeof(keyQ), "BidLevel[%d].Qty",   i);
                bid[i].Price = dict.at(keyP);
                bid[i].Qty   = dict.at(keyQ);
                snprintf(keyP, sizeof(keyP), "AskLevel[%d].Price", i);
                snprintf(keyQ, sizeof(keyQ), "AskLevel[%d].Qty",   i);
                ask[i].Price = dict.at(keyP);
                ask[i].Qty   = dict.at(keyQ);
            }
        }
    }

    // ---- 从 TradingPhaseCode 解析市场交易阶段 ----
    TPM tradingPhaseMarket() const {
        if (secSrc == SecurityIDSource_SZSE) {
            int code0 = TradingPhaseCode & 0x0F;
            switch (code0) {
                case 0: return TPM::Starting;
                case 1: return TPM::OpenCall;
                case 2: return (HHMMSSms() < 120000000) ? TPM::AMTrading : TPM::PMTrading;
                case 3: return (HHMMSSms() < 93100000)  ? TPM::PreTradingBreaking : TPM::Breaking;
                case 4: return TPM::CloseCall;
                case 5: return TPM::Ending;
                case 6: return TPM::HangingUp;
                case 7: return TPM::AfterCloseTrading;
                case 8: return TPM::VolatilityBreaking;
                default: return TPM::Unknown;
            }
        }
        return TPM::Unknown;
    }

    TPI tradingPhaseSecurity() const {
        if (secSrc == SecurityIDSource_SZSE) {
            int code1 = TradingPhaseCode >> 4;
            if (code1 == 0) return TPI::Normal;
            if (code1 == 1) return TPI::NoTrade;
        }
        return TPI::Unknown;
    }

    void updateTradingPhaseCode(TPM tpm, TPI tpi) {
        uint8_t code0 = 0xF, code1 = 0xF;
        switch (tpm) {
            case TPM::Starting:           code0 = 0; break;
            case TPM::OpenCall:           code0 = 1; break;
            case TPM::AMTrading:
            case TPM::PMTrading:          code0 = 2; break;
            case TPM::PreTradingBreaking:
            case TPM::Breaking:           code0 = 3; break;
            case TPM::CloseCall:          code0 = 4; break;
            case TPM::Ending:             code0 = 5; break;
            case TPM::HangingUp:          code0 = 6; break;
            case TPM::AfterCloseTrading:  code0 = 7; break;
            case TPM::VolatilityBreaking: code0 = 8; break;
            default: break;
        }
        switch (tpi) {
            case TPI::Normal:  code1 = 0; break;
            case TPI::NoTrade: code1 = 1; break;
            default: break;
        }
        TradingPhaseCode = (code1 << 4) | code0;
    }

    uint64_t HHMMSSms() const {
        if (secSrc == SecurityIDSource_SZSE)
            return TransactTime % 1000000000ULL;
        return TransactTime;
    }

    // ---- 比较两个快照是否相同（验证用）----
    bool isSame(const AxsbeSnapStock& o) const {
        if (NumTrades        != o.NumTrades)        return false;
        if (TotalVolumeTrade != o.TotalVolumeTrade) return false;
        if (TotalValueTrade  != o.TotalValueTrade)  return false;
        if (PrevClosePx      != o.PrevClosePx)      return false;
        if (LastPx           != o.LastPx)           return false;
        if (OpenPx           != o.OpenPx)           return false;
        if (HighPx           != o.HighPx)           return false;
        if (LowPx            != o.LowPx)            return false;
        if (UpLimitPx        != o.UpLimitPx)        return false;
        if (DnLimitPx        != o.DnLimitPx)        return false;
        for (int i = 0; i < 10; i++) {
            if (bid[i] != o.bid[i]) return false;
            if (ask[i] != o.ask[i]) return false;
        }
        return true;
    }

    std::string toString() const {
        char buf[1024];
        int n = snprintf(buf, sizeof(buf),
            "%s %06d NumTrades=%lld TVol=%lld TVal=%lld Last=%lld O=%lld H=%lld L=%lld",
            _source, securityID,
            (long long)NumTrades, (long long)TotalVolumeTrade,
            (long long)TotalValueTrade, (long long)LastPx,
            (long long)OpenPx, (long long)HighPx, (long long)LowPx);
        for (int i = 0; i < 3 && n < (int)sizeof(buf)-1; i++) {
            n += snprintf(buf+n, sizeof(buf)-n,
                "\n  Ask[%d]=%lld*%lld  Bid[%d]=%lld*%lld",
                i, (long long)ask[i].Price, (long long)ask[i].Qty,
                i, (long long)bid[i].Price, (long long)bid[i].Qty);
        }
        return buf;
    }
};
