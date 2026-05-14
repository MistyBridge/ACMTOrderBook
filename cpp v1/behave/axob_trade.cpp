#include "axob.h"
#include <cstdio>
#include <cassert>

void AXOB::onMsg(const AxsbeExe& msg) {
    if (msg.securityID != SecurityID) return;
    if (cageType == CageType::CYB && tradingPhase == TPM::PMTrading && msg.tradingPhaseMarket() == TPM::CloseCall) {
        openCage();
    }
    useTimestamp(msg.TransactTime);
    if (tradingPhase != TPM::VolatilityBreaking) {
        tradingPhase = msg.tradingPhaseMarket();
    }
    onExec(msg);
    msgNb++;
}

void AXOB::onExec(const AxsbeExe& rawExec) {
    if (rawExec.isTrade() || secSrc == SecurityIDSource_SSE) {
        ObExec exec(rawExec, instType);
        onTrade(exec);
    } else {
        uint64_t cancelSeq = 0;
        Side cancelSide = Side::UNKNOWN;
        if (rawExec.BidApplSeqNum != 0) {
            cancelSeq  = rawExec.BidApplSeqNum;
            cancelSide = Side::BID;
        } else {
            cancelSeq  = rawExec.OfferApplSeqNum;
            cancelSide = Side::ASK;
        }
        ObCancel cancel;
        cancel.applSeqNum   = cancelSeq;
        cancel.qty          = static_cast<int32_t>(rawExec.LastQty);
        cancel.price        = 0;
        cancel.side         = cancelSide;
        cancel.TransactTime = rawExec.TransactTime;
        onCancel(cancel);
    }
}

void AXOB::onTrade(const ObExec& exec) {
    NumTrades++;
    TotalVolumeTrade += exec.LastQty;

    if (secSrc == SecurityIDSource_SZSE) {
        if (instType == InstrumentType::STOCK)
            TotalValueTrade += (int64_t)exec.LastQty * exec.LastPx / (QTY_INTER_SZSE_PRECISION * PRICE_INTER_STOCK_PRECISION / TOTALVALUETRADE_SZSE_PRECISION);
        else if (instType == InstrumentType::FUND)
            TotalValueTrade += (int64_t)exec.LastQty * exec.LastPx / (QTY_INTER_SZSE_PRECISION * PRICE_INTER_FUND_PRECISION / TOTALVALUETRADE_SZSE_PRECISION);
        else if (instType == InstrumentType::KZZ)
            TotalValueTrade += (int64_t)exec.LastQty * exec.LastPx / (QTY_INTER_SZSE_PRECISION * PRICE_INTER_KZZ_PRECISION / TOTALVALUETRADE_SZSE_PRECISION);
    } else if (secSrc == SecurityIDSource_SSE) {
        if (instType == InstrumentType::STOCK)
            TotalValueTrade += (int64_t)exec.LastQty * exec.LastPx / (QTY_INTER_SSE_PRECISION * PRICE_INTER_STOCK_PRECISION / TOTALVALUETRADE_SSE_PRECISION);
    }

    LastPx = exec.LastPx;
    if (OpenPx == 0) { OpenPx = exec.LastPx; HighPx = exec.LastPx; LowPx = exec.LastPx; }
    else { if (HighPx < exec.LastPx) HighPx = exec.LastPx; if (LowPx > exec.LastPx) LowPx = exec.LastPx; }

    if (holdingNb && holdingOrder && holdingOrder->type == OrdType::MARKET) {
        if (holdingOrder->applSeqNum != exec.BidApplSeqNum && holdingOrder->applSeqNum != exec.OfferApplSeqNum) {
            fprintf(stderr, "%06d MARKET order followed by unmatch exec\n", SecurityID);
            insertOrder(*holdingOrder);
            holdingNb = 0;
            useTimestamp(holdingOrder->TransactTime);
            genSnap();
            useTimestamp(exec.TransactTime);
        }
    }

    if (holdingNb != 0 && holdingOrder) {
        Side levelSide = (exec.BidApplSeqNum == holdingOrder->applSeqNum) ? Side::ASK : Side::BID;
        if (holdingOrder->qty == exec.LastQty) { holdingNb = 0; }
        else {
            holdingOrder->qty -= exec.LastQty;
            if (holdingOrder->type == OrdType::MARKET) { holdingOrder->price = exec.LastPx; holdingOrder->traded = true; }
        }
        if (levelSide == Side::ASK) tradeLimit(Side::ASK, exec.LastQty, exec.OfferApplSeqNum);
        else tradeLimit(Side::BID, exec.LastQty, exec.BidApplSeqNum);

        if (holdingNb != 0 && holdingOrder->type == OrdType::LIMIT) {
            if ((holdingOrder->side == Side::BID && (holdingOrder->price < askMinPrice || askMinQty == 0)) ||
                (holdingOrder->side == Side::ASK && (holdingOrder->price > bidMaxPrice || bidMaxQty == 0))) {
                insertOrder(*holdingOrder);
                holdingNb = 0;
            }
        }
        if (cageType == CageType::CYB) enterCage();
        if (holdingNb == 0) genSnap();

    } else if (bidWaitingForCage || askWaitingForCage) {
        tradeLimit(Side::ASK, exec.LastQty, exec.OfferApplSeqNum);
        tradeLimit(Side::BID, exec.LastQty, exec.BidApplSeqNum);
        if (cageType == CageType::CYB) enterCage();
        genSnap();

    } else {
        tradeLimit(Side::ASK, exec.LastQty, exec.OfferApplSeqNum);
        tradeLimit(Side::BID, exec.LastQty, exec.BidApplSeqNum);
        if (askMinQty == 0 || bidMaxQty == 0 || askMinPrice > bidMaxPrice) {
            genSnap();
        }
    }
}

void AXOB::onCancel(const ObCancel& cancel) {
    if (holdingNb != 0) {
        holdingNb = 0;
        if (holdingOrder) {
            useTimestamp(holdingOrder->TransactTime);
            genSnap();
            useTimestamp(cancel.TransactTime);
        }
    }
    auto it = orderMap.find(cancel.applSeqNum);
    if (it == orderMap.end()) return;
    ObOrder& order = it->second;
    levelDequeue(order.side, order.price, cancel.qty, cancel.applSeqNum);
}

void AXOB::tradeLimit(Side side, int32_t qty, uint64_t applSeqNum) {
    auto it = orderMap.find(applSeqNum);
    if (it == orderMap.end()) {
        fprintf(stderr, "%06d traded order not found\n", SecurityID);
        return;
    }
    levelDequeue(side, it->second.price, qty, applSeqNum);
}

void AXOB::levelDequeue(Side side, int32_t price, int32_t qty, uint64_t applSeqNum) {
    if (side == Side::BID) {
        auto it = bidLevelTree.find(price);
        if (it == bidLevelTree.end()) return;
        it->second.qty -= qty;
        if (price == bidMaxPrice) bidMaxQty -= qty;

        if (bidCageUpperExMinQty == 0 || price < bidCageUpperExMinPrice) {
            BidWeightSize  -= qty;
            BidWeightValue -= (int64_t)price * qty;
        } else if (price == bidCageUpperExMinPrice) {
            bidCageUpperExMinQty -= qty;
            if (bidCageUpperExMinQty == 0) {
                for (auto& [p, l] : bidLevelTree) {
                    if (p > bidCageUpperExMinPrice) {
                        bidCageUpperExMinPrice = p;
                        bidCageUpperExMinQty   = l.qty;
                        break;
                    }
                }
            }
        }

        if (it->second.qty == 0) {
            bidLevelTree.erase(it);
            if (price == bidMaxPrice) {
                bidMaxQty = 0;
                for (auto rit = bidLevelTree.rbegin(); rit != bidLevelTree.rend(); ++rit) {
                    if (rit->first < price) {
                        bidMaxPrice = rit->first;
                        bidMaxQty   = rit->second.qty;
                        break;
                    }
                }
                if (bidMaxQty != 0)      askCageRefPx = bidMaxPrice;
                else if (askMinQty != 0) askCageRefPx = askMinPrice;
                else                     askCageRefPx = LastPx;

                if (tradingPhase == TPM::AMTrading || tradingPhase == TPM::PMTrading)
                    askWaitingForCage = (cageType == CageType::CYB);
                else
                    askWaitingForCage = false;
            }
        }
    } else {
        auto it = askLevelTree.find(price);
        if (it == askLevelTree.end()) return;
        it->second.qty -= qty;
        if (price == askMinPrice) askMinQty -= qty;

        if (askCageLowerExMaxQty == 0 || price > askCageLowerExMaxPrice) {
            if (tradingPhase == TPM::OpenCall && price > mktInfo.PrevClosePx * 10) {
                AskWeightSizeEx  -= qty;
                AskWeightValueEx -= (int64_t)price * qty;
            } else {
                AskWeightSize  -= qty;
                AskWeightValue -= (int64_t)price * qty;
            }
        } else if (price == askCageLowerExMaxPrice) {
            askCageLowerExMaxQty -= qty;
            if (askCageLowerExMaxQty == 0) {
                for (auto rit = askLevelTree.rbegin(); rit != askLevelTree.rend(); ++rit) {
                    if (rit->first < askCageLowerExMaxPrice) {
                        askCageLowerExMaxPrice = rit->first;
                        askCageLowerExMaxQty   = rit->second.qty;
                        break;
                    }
                }
            }
        }

        if (it->second.qty == 0) {
            askLevelTree.erase(it);
            if (price == askMinPrice) {
                askMinQty = 0;
                for (auto& [p, l] : askLevelTree) {
                    if (p > price) {
                        askMinPrice = p;
                        askMinQty   = l.qty;
                        break;
                    }
                }
                if (askMinQty != 0)      bidCageRefPx = askMinPrice;
                else if (bidMaxQty != 0) bidCageRefPx = bidMaxPrice;
                else                     bidCageRefPx = LastPx;

                if (tradingPhase == TPM::AMTrading || tradingPhase == TPM::PMTrading)
                    bidWaitingForCage = (cageType == CageType::CYB);
                else
                    bidWaitingForCage = false;
            }
        }
    }
    // 注意：Python 版本从不从 orderMap 删除订单！
    // orderMap 保留所有历史订单，用于后续成交查找。
}
