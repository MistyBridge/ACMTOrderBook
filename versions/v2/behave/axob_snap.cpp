#include "axob.h"
#include <cstdio>
#include <cmath>
#include <map>

static int32_t fmtPx(int32_t price, InstrumentType instType, SecurityIDSource src) {
    return fmtPriceInter2Snap(price, instType, src);
}

// 消息入口
void AXOB::onMsg(const AxsbeSnapStock& msg) {
    if (msg.securityID != SecurityID) return;
    onSnap(msg);
    msgNb++;
}

void AXOB::onMsg(AXSignal signal) {
    switch (signal) {
    case AXSignal::OPENCALL_END:
        if (bidMaxPrice < askMinPrice && tradingPhase == TPM::OpenCall) {
            tradingPhase = TPM::PreTradingBreaking;
            genSnap();
        }
        break;
    case AXSignal::AMTRADING_BGN:
        if (tradingPhase == TPM::PreTradingBreaking) {
            AskWeightSize  += AskWeightSizeEx;
            AskWeightValue += AskWeightValueEx;
            tradingPhase = TPM::AMTrading;
            genSnap();
        }
        break;
    case AXSignal::AMTRADING_END:
        if (tradingPhase == TPM::AMTrading && !hasHoldingOrder_) {
            tradingPhase = TPM::Breaking;
            genSnap();
        }
        break;
    case AXSignal::PMTRADING_END:
        if (!hasHoldingOrder_ && tradingPhase == TPM::PMTrading) {
            tradingPhase = TPM::CloseCall;
            openCage();
            genSnap();
        }
        break;
    case AXSignal::ALL_END:
        if (bidMaxPrice < askMinPrice && tradingPhase == TPM::CloseCall) {
            tradingPhase = TPM::Ending;
            closePxReady = false;
        } else {
            closePxReady = true;
            genSnap();
        }
        break;
    case AXSignal::VB_BGN:
        tradingPhase = TPM::VolatilityBreaking;
        break;
    case AXSignal::VB_END:
        tradingPhase = TPM::AMTrading;  // 或 PMTrading，取决于当前阶段
        break;
    default: break;
    }
}

// 快照验证
void AXOB::onSnap(const AxsbeSnapStock& snap) {
    if (snap.tradingPhaseSecurity() != TPI::Normal) return;

    if (mktInfo.ChannelNo == 0) {
        mktInfo.ChannelNo    = snap.ChannelNo;
        mktInfo.UpLimitPx    = snap.UpLimitPx;
        mktInfo.DnLimitPx    = snap.DnLimitPx;

        if (secSrc == SecurityIDSource_SZSE) {
            if (instType == InstrumentType::STOCK)
                mktInfo.PrevClosePx = snap.PrevClosePx / (PRICE_SZSE_SNAP_PRECLOSE_PRECISION / PRICE_INTER_STOCK_PRECISION);
            else if (instType == InstrumentType::FUND)
                mktInfo.PrevClosePx = snap.PrevClosePx / (PRICE_SZSE_SNAP_PRECLOSE_PRECISION / PRICE_INTER_FUND_PRECISION);
        }
        bidCageRefPx = mktInfo.PrevClosePx;
        askCageRefPx = mktInfo.PrevClosePx;

        if (secSrc == SecurityIDSource_SZSE) {
            if (instType == InstrumentType::STOCK) {
                mktInfo.UpLimitPrice = snap.UpLimitPx / (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_STOCK_PRECISION);
                mktInfo.DnLimitPrice = snap.DnLimitPx / (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_STOCK_PRECISION);
            } else if (instType == InstrumentType::FUND) {
                mktInfo.UpLimitPrice = snap.UpLimitPx / (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_FUND_PRECISION);
                mktInfo.DnLimitPrice = snap.DnLimitPx / (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_FUND_PRECISION);
            }
            mktInfo.YYMMDD = snap.TransactTime / SZSE_TICK_CUT;
        }
    }

    // 收盘价补充
    if (tradingPhase == TPM::Ending && !closePxReady) {
        if (secSrc == SecurityIDSource_SZSE) {
            if (instType == InstrumentType::STOCK)
                LastPx = snap.LastPx / (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_STOCK_PRECISION);
            else if (instType == InstrumentType::FUND)
                LastPx = snap.LastPx / (PRICE_SZSE_SNAP_PRECISION / PRICE_INTER_FUND_PRECISION);
        }
        closePxReady = true;
        genSnap();
    }

    // 波动性中断
    if (snap.tradingPhaseMarket() == TPM::VolatilityBreaking && tradingPhase != TPM::VolatilityBreaking) {
        volatilityBreakingEndTick = 0;
        tradingPhase = TPM::VolatilityBreaking;
        genSnap();
    }
}

// 根据交易阶段分发到对应快照生成器
void AXOB::genSnap() {
    AxsbeSnapStock snap;
    snap.secSrc = secSrc;
    snap.securityID = SecurityID;

    if (tradingPhase < TPM::OpenCall || tradingPhase > TPM::Ending) return;

    if (tradingPhase == TPM::OpenCall || tradingPhase == TPM::CloseCall) {
        snap = genCallSnap();
    } else if (tradingPhase == TPM::VolatilityBreaking) {
        snap = genTradingSnap(true);
    } else if (tradingPhase == TPM::Ending) {
        if (closePxReady) snap = genTradingSnap();
        else return;
    } else {
        snap = genTradingSnap();
    }

    clipSnap(snap);
    snap._seq = msgNb;
    delete lastSnap;
    lastSnap = new AxsbeSnapStock(snap);
}

// 集合竞价快照 — 虚拟撮合算法
AxsbeSnapStock AXOB::genCallSnap(int showLevelNb) {
    int32_t _bid_p = bidMaxPrice, _bid_q = bidMaxQty;
    int32_t _ask_p = askMinPrice, _ask_q = askMinQty;

    // 初始撮合价
    int32_t price = 0;
    if (_bid_q == 0 && _ask_q == 0)      price = 0;
    else if (_bid_q == 0)                 price = _ask_p;
    else if (_ask_q == 0)                 price = _bid_p;

    int32_t volumeTrade = 0, bidQty = 0, askQty = 0;
    int32_t refPx = (NumTrades == 0) ? mktInfo.PrevClosePx : LastPx;

    // 撮合循环
    while (_bid_q != 0 && _ask_q != 0) {
        if (_bid_p >= _ask_p) {
            if (bidQty == 0) bidQty = _bid_q;
            if (askQty == 0) askQty = _ask_q;

            if (bidQty >= askQty) {
                volumeTrade += askQty;
                bidQty -= askQty;
                askQty = 0;
            } else {
                volumeTrade += bidQty;
                askQty -= bidQty;
                bidQty = 0;
            }

            if (bidQty == 0 && askQty == 0) {
                if (_bid_p >= refPx && _ask_p <= refPx) price = refPx;
                else if (abs(_bid_p - refPx) < abs(_ask_p - refPx)) price = _bid_p;
                else price = _ask_p;
            }

            if (bidQty == 0) {
                if (askQty != 0) price = _ask_p;
                _bid_q = 0;
                for (auto rit = bidLevelTree.rbegin(); rit != bidLevelTree.rend(); ++rit) {
                    if (rit->first < _bid_p) { _bid_p = rit->first; _bid_q = rit->second.qty; break; }
                }
            }
            if (askQty == 0) {
                if (bidQty != 0) price = _bid_p;
                _ask_q = 0;
                for (auto& [p, l] : askLevelTree) {
                    if (p > _ask_p) { _ask_p = p; _ask_q = l.qty; break; }
                }
            }
        } else {
            // 无交叉，修正成交价
            if (askQty == 0 && bidQty == 0) {
                if (_ask_q && price >= _ask_p) {
                    if (_bid_p + 1 < _ask_p) price = _ask_p - 1;
                    else {
                        if (_ask_q <= _bid_q) { price = _ask_p; askQty = _ask_q; }
                        else { price = _bid_p; bidQty = _bid_q; }
                    }
                } else if (_bid_q && price <= _bid_p) {
                    if (_ask_p > _bid_p + 1) price = _bid_p + 1;
                    else {
                        if (_bid_q <= _ask_q) { price = _bid_p; bidQty = _bid_q; }
                        else { price = _ask_p; askQty = _ask_q; }
                    }
                }
            }
            break;
        }
    }

    // 构造快照
    AxsbeSnapStock snap;
    snap.secSrc = secSrc;
    snap.securityID = SecurityID;
    setSnapFixParam(snap);

    int32_t snapPrice = fmtPx(price, instType, secSrc);
    if (volumeTrade == 0) {
        for (int i = 0; i < showLevelNb; i++) { snap.bid[i] = PriceLevel(0,0); snap.ask[i] = PriceLevel(0,0); }
    } else {
        snap.bid[0] = PriceLevel(snapPrice, volumeTrade);
        snap.ask[0] = PriceLevel(snapPrice, volumeTrade);
        snap.bid[1] = PriceLevel(0, bidQty);
        snap.ask[1] = PriceLevel(0, askQty);
        for (int i = 2; i < showLevelNb; i++) { snap.bid[i] = PriceLevel(0,0); snap.ask[i] = PriceLevel(0,0); }
    }

    snap.NumTrades        = NumTrades;
    snap.TotalVolumeTrade = TotalVolumeTrade;
    snap.TotalValueTrade  = TotalValueTrade;
    snap.LastPx  = fmtPx(LastPx,  instType, secSrc);
    snap.HighPx  = fmtPx(HighPx,  instType, secSrc);
    snap.LowPx   = fmtPx(LowPx,   instType, secSrc);
    snap.OpenPx  = fmtPx(OpenPx,  instType, secSrc);
    snap.BidWeightPx = 0; snap.BidWeightSize = 0;
    snap.AskWeightPx = 0; snap.AskWeightSize = 0;
    setSnapTimestamp(snap);
    snap.updateTradingPhaseCode(tradingPhase, TPI::Normal);
    return snap;
}

// 连续竞价快照
AxsbeSnapStock AXOB::genTradingSnap(bool isVolBreaking, int levelNb) {
    AxsbeSnapStock snap;
    snap.secSrc = secSrc;
    snap.securityID = SecurityID;

    // 买方档位（从大到小）
    int lv = 0;
    if (!isVolBreaking) {
        for (auto rit = bidLevelTree.rbegin(); rit != bidLevelTree.rend() && lv < levelNb; ++rit) {
            if (bidCageUpperExMinQty == 0 || rit->first < bidCageUpperExMinPrice) {
                snap.bid[lv++] = PriceLevel(fmtPx(rit->first, instType, secSrc), rit->second.qty);
            }
        }
    }
    for (int i = lv; i < levelNb; i++) snap.bid[i] = PriceLevel(0, 0);

    // 卖方档位（从小到大）
    lv = 0;
    if (!isVolBreaking) {
        for (auto& [p, l] : askLevelTree) {
            if (lv >= levelNb) break;
            if (askCageLowerExMaxQty == 0 || p > askCageLowerExMaxPrice) {
                snap.ask[lv++] = PriceLevel(fmtPx(p, instType, secSrc), l.qty);
            }
        }
    }
    for (int i = lv; i < levelNb; i++) snap.ask[i] = PriceLevel(0, 0);

    setSnapFixParam(snap);
    snap.NumTrades        = NumTrades;
    snap.TotalVolumeTrade = TotalVolumeTrade;
    snap.TotalValueTrade  = TotalValueTrade;
    snap.LastPx = fmtPx(LastPx, instType, secSrc);
    snap.HighPx = fmtPx(HighPx, instType, secSrc);
    snap.LowPx  = fmtPx(LowPx,  instType, secSrc);
    snap.OpenPx = fmtPx(OpenPx, instType, secSrc);

    if (isVolBreaking) {
        snap.BidWeightPx = 0; snap.BidWeightSize = 0;
        snap.AskWeightPx = 0; snap.AskWeightSize = 0;
    } else {
        if (BidWeightSize != 0) {
            snap.BidWeightPx = (int32_t)(((BidWeightValue << 1) / BidWeightSize + 1) >> 1);
            snap.BidWeightPx = fmtPx(snap.BidWeightPx, instType, secSrc);
        } else snap.BidWeightPx = 0;
        snap.BidWeightSize = BidWeightSize;

        if (AskWeightSize != 0) {
            snap.AskWeightPx = (int32_t)(((AskWeightValue << 1) / AskWeightSize + 1) >> 1);
            snap.AskWeightPx = fmtPx(snap.AskWeightPx, instType, secSrc);
        } else snap.AskWeightPx = 0;
        snap.AskWeightSize = AskWeightSize;
    }

    setSnapTimestamp(snap);
    snap.updateTradingPhaseCode(tradingPhase, TPI::Normal);
    return snap;
}
