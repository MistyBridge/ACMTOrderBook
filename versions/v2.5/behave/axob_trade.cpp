#include "axob.h"
#include <cstdio>
#include <cassert>

void AXOB::onMsg(const AxsbeExe& msg) {
    if (UNLIKELY(msg.securityID != SecurityID)) return;
    // [v2优化] CYB 收盘集合竞价进入是罕见路径
    if (UNLIKELY(cageType == CageType::CYB && tradingPhase == TPM::PMTrading && msg.tradingPhaseMarket() == TPM::CloseCall)) {
        openCage();
    }
    useTimestamp(msg.TransactTime);
    if (LIKELY(tradingPhase != TPM::VolatilityBreaking)) {
        tradingPhase = msg.tradingPhaseMarket();
    }
    onExec(msg);
    msgNb++;
}

// [v2优化] 成交是热路径（~80%的Exe消息），撤单是较冷路径
void AXOB::onExec(const AxsbeExe& rawExec) {
    if (LIKELY(rawExec.isTrade() || secSrc == SecurityIDSource_SSE)) {
        ObExec exec(rawExec, instType);
        onTrade(exec);
    } else {
        // 撤单路径
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

    // [v2优化] 深交所股票是热路径
    if (LIKELY(secSrc == SecurityIDSource_SZSE)) {
        if (LIKELY(instType == InstrumentType::STOCK))
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
    // [v2优化] OpenPx==0 仅首笔成交时为真
    if (UNLIKELY(OpenPx == 0)) { OpenPx = exec.LastPx; HighPx = exec.LastPx; LowPx = exec.LastPx; }
    else { if (HighPx < exec.LastPx) HighPx = exec.LastPx; if (LowPx > exec.LastPx) LowPx = exec.LastPx; }

    // [v2优化] holdingOrder_ 现在是栈对象，不需要 nullptr 检查
    if (hasHoldingOrder_ && UNLIKELY(holdingOrder_.type == OrdType::MARKET)) {
        if (holdingOrder_.applSeqNum != exec.BidApplSeqNum && holdingOrder_.applSeqNum != exec.OfferApplSeqNum) {
            fprintf(stderr, "%06d MARKET order followed by unmatch exec\n", SecurityID);
            insertOrder(holdingOrder_);
            hasHoldingOrder_ = false;
            useTimestamp(holdingOrder_.TransactTime);
            genSnap();
            useTimestamp(exec.TransactTime);
        }
    }

    if (hasHoldingOrder_) {
        Side levelSide = (exec.BidApplSeqNum == holdingOrder_.applSeqNum) ? Side::ASK : Side::BID;
        if (holdingOrder_.qty == exec.LastQty) { hasHoldingOrder_ = false; }
        else {
            holdingOrder_.qty -= exec.LastQty;
            if (UNLIKELY(holdingOrder_.type == OrdType::MARKET)) { holdingOrder_.price = exec.LastPx; holdingOrder_.traded = true; }
        }
        if (levelSide == Side::ASK) tradeLimit(Side::ASK, exec.LastQty, exec.OfferApplSeqNum);
        else tradeLimit(Side::BID, exec.LastQty, exec.BidApplSeqNum);

        if (hasHoldingOrder_ && holdingOrder_.type == OrdType::LIMIT) {
            if ((holdingOrder_.side == Side::BID && (holdingOrder_.price < askMinPrice || askMinQty == 0)) ||
                (holdingOrder_.side == Side::ASK && (holdingOrder_.price > bidMaxPrice || bidMaxQty == 0))) {
                insertOrder(holdingOrder_);
                hasHoldingOrder_ = false;
            }
        }
        if (cageType == CageType::CYB) enterCage();
        if (!hasHoldingOrder_) genSnap();

    } else if (bidWaitingForCage || askWaitingForCage) {
        tradeLimit(Side::ASK, exec.LastQty, exec.OfferApplSeqNum);
        tradeLimit(Side::BID, exec.LastQty, exec.BidApplSeqNum);
        if (cageType == CageType::CYB) enterCage();
        genSnap();

    } else {
        // [v2优化] 最常见路径：无缓存单、无笼子等待
        tradeLimit(Side::ASK, exec.LastQty, exec.OfferApplSeqNum);
        tradeLimit(Side::BID, exec.LastQty, exec.BidApplSeqNum);
        if (UNLIKELY(askMinQty == 0 || bidMaxQty == 0 || askMinPrice > bidMaxPrice)) {
            genSnap();
        }
    }

    // [v2.7] 增量更新快照统计字段（~5ns），避免完整重建
    updateSnapStats();
}

void AXOB::onCancel(const ObCancel& cancel) {
    if (hasHoldingOrder_) {
        hasHoldingOrder_ = false;
        useTimestamp(holdingOrder_.TransactTime);
        genSnap();
        useTimestamp(cancel.TransactTime);
    }
    // [v2优化] orderMap 现在存指针，需要解引用
    auto it = orderMap.find(cancel.applSeqNum);
    if (it == orderMap.end()) return;
    ObOrder& order = *(it->second);
    levelDequeue(order.side, order.price, cancel.qty, cancel.applSeqNum);
}

// [v2优化] tradeLimit: orderMap 存 ObOrder* 后需解引用，用 find() 单次查找
void AXOB::tradeLimit(Side side, int32_t qty, uint64_t applSeqNum) {
    auto it = orderMap.find(applSeqNum);
    if (UNLIKELY(it == orderMap.end())) {
        fprintf(stderr, "%06d traded order not found\n", SecurityID);
        return;
    }
    levelDequeue(side, it->second->price, qty, applSeqNum);
}

void AXOB::levelDequeue(Side side, int32_t price, int32_t qty, [[maybe_unused]] uint64_t applSeqNum) {
    if (side == Side::BID) {
        auto* node = bidLevelBook.find(price);
        if (!node) return;
        node->qty -= qty;
        if (price == bidMaxPrice) bidMaxQty -= qty;

        if (bidCageUpperExMinQty == 0 || price < bidCageUpperExMinPrice) {
            BidWeightSize  -= qty;
            BidWeightValue -= (int64_t)price * qty;
        } else if (price == bidCageUpperExMinPrice) {
            bidCageUpperExMinQty -= qty;
            if (bidCageUpperExMinQty == 0) {
                for (int i = 0; i < bidLevelBook.count; i++) {
                    if (bidLevelBook.levels[i].price > bidCageUpperExMinPrice) {
                        bidCageUpperExMinPrice = bidLevelBook.levels[i].price;
                        bidCageUpperExMinQty   = bidLevelBook.levels[i].qty;
                        break;
                    }
                }
            }
        }

        if (node->qty == 0) {
            bidLevelBook.erase(price);
            if (price == bidMaxPrice) {
                bidMaxQty = 0;
                // [v2.2优化] O(1) 直接取最优价：升序数组末尾 = 最大买价
                // erase 已移除原最优价，levels[count-1] 必然 < 原 price
                if (bidLevelBook.count > 0) {
                    bidMaxPrice = bidLevelBook.levels[bidLevelBook.count - 1].price;
                    bidMaxQty   = bidLevelBook.levels[bidLevelBook.count - 1].qty;
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
        auto* node = askLevelBook.find(price);
        if (!node) return;
        node->qty -= qty;
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
                // CompactLevelBook 升序，反向找更低价格
                for (int i = askLevelBook.count - 1; i >= 0; i--) {
                    if (askLevelBook.levels[i].price < askCageLowerExMaxPrice) {
                        askCageLowerExMaxPrice = askLevelBook.levels[i].price;
                        askCageLowerExMaxQty   = askLevelBook.levels[i].qty;
                        break;
                    }
                }
            }
        }

        if (node->qty == 0) {
            askLevelBook.erase(price);
            if (price == askMinPrice) {
                askMinQty = 0;
                // [v2.2优化] O(1) 直接取最优价：升序数组开头 = 最小卖价
                // erase 已移除原最优价，levels[0] 必然 > 原 price
                if (askLevelBook.count > 0) {
                    askMinPrice = askLevelBook.levels[0].price;
                    askMinQty   = askLevelBook.levels[0].qty;
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
