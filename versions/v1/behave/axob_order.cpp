#include "axob.h"

// =====================================================================
//  委托处理：onMsg(order) -> onOrder -> onLimitOrder -> insertOrder
//  对应 Python axob.py L470-508, L655-828
// =====================================================================

// ---- 消息入口：逐笔委托 ----
void AXOB::onMsg(const AxsbeOrder& msg) {
    if (msg.securityID != SecurityID) return;

    // CYB 进入收盘集合竞价，敞开价格笼子
    if (mktSubType == MarketSubType::SZSE_STK_GEM &&
        tradingPhase == TPM::PMTrading &&
        msg.tradingPhaseMarket() == TPM::CloseCall) {
        openCage();
    }

    useTimestamp(msg.TransactTime);

    if (tradingPhase != TPM::VolatilityBreaking) {
        tradingPhase = msg.tradingPhaseMarket();
    }

    onOrder(msg);
    msgNb++;
}

// ---- 提取字段，处理市价/本方最优，调用 onLimitOrder ----
void AXOB::onOrder(const AxsbeOrder& rawOrder) {
    ObOrder order(rawOrder, instType);

    if (order.type == OrdType::MARKET) {
        // 市价单：后续由成交消息确定价格
        // 暂不处理，等后续 onTrade
    } else if (order.type == OrdType::SIDE) {
        // 本方最优 -> 转限价单
        if (order.side == Side::BID) {
            if (bidMaxPrice != 0 && bidMaxQty != 0)
                order.price = bidMaxPrice;
            else
                order.price = mktInfo.DnLimitPrice;
        } else {
            if (askMinPrice != 0 && askMinQty != 0)
                order.price = askMinPrice;
            else
                order.price = mktInfo.UpLimitPrice;
        }
    }

    onLimitOrder(order);
}

// ---- 限价单处理 ----
void AXOB::onLimitOrder(ObOrder& order) {
    if (tradingPhase == TPM::OpenCall || tradingPhase == TPM::CloseCall) {
        // 集合竞价期间：直接插入
        if (tradingPhase == TPM::CloseCall && holdingNb != 0) {
            insertOrder(*holdingOrder);
            holdingNb = 0;
        }

        // 创业板上市头5日超出范围则丢弃
        if (tradingPhase == TPM::CloseCall && mktInfo.UpLimitPx == PRICE_MAXIMUM &&
            (order.price > cybMatchUpper(LastPx) || order.price < cybMatchLower(LastPx))) {
            // 跳过
        } else {
            insertOrder(order);
            bidWaitingForCage = false;
            askWaitingForCage = false;
        }

        genSnap();
    } else {
        // 连续竞价
        if (holdingNb != 0) {
            // 先把此前缓存的订单插入LOB
            if (holdingOrder->type == OrdType::MARKET && !holdingOrder->traded) {
                fprintf(stderr, "%06d MARKET order not followed by trade!\n", SecurityID);
            }
            insertOrder(*holdingOrder);
            holdingNb = 0;
            useTimestamp(holdingOrder->TransactTime);
            genSnap();
            useTimestamp(order.TransactTime);
        }

        // CYB 价格笼子判断
        if (mktSubType == MarketSubType::SZSE_STK_GEM && order.type == OrdType::LIMIT &&
            ((order.side == Side::BID && order.price > cybCageUpper(bidCageRefPx)) ||
             (order.side == Side::ASK && order.price < cybCageLower(askCageRefPx)))) {
            insertOrder(order, true);
            genSnap();
        } else if (tradingPhase == TPM::VolatilityBreaking) {
            insertOrder(order);
            genSnap();
        } else {
            // 市价单或可成交限价单 -> 缓存
            if (order.type == OrdType::MARKET) {
                holdingOrder = std::make_unique<ObOrder>(order);
                holdingNb++;
            } else if ((order.side == Side::BID && order.price >= askMinPrice && askMinQty > 0) ||
                       (order.side == Side::ASK && order.price <= bidMaxPrice && bidMaxQty > 0)) {
                holdingOrder = std::make_unique<ObOrder>(order);
                holdingNb++;
                bidWaitingForCage = false;
                askWaitingForCage = false;
            } else {
                insertOrder(order);
                if (mktSubType == MarketSubType::SZSE_STK_GEM) {
                    enterCage();
                }
                genSnap();
            }
        }
    }
}

// ---- 订单入列 ----
void AXOB::insertOrder(const ObOrder& order, bool outOfCage) {
    orderMap[order.applSeqNum] = order;

    if (order.side == Side::BID) {
        auto it = bidLevelTree.find(order.price);
        if (it != bidLevelTree.end()) {
            it->second.qty += order.qty;
            if (order.price == bidMaxPrice)
                bidMaxQty += order.qty;
            if (bidCageUpperExMinQty && order.price == bidCageUpperExMinPrice)
                bidCageUpperExMinQty += order.qty;
        } else {
            bidLevelTree[order.price] = LevelNode(order.price, order.qty);
            if (!outOfCage) {
                if (bidMaxQty == 0 || order.price > bidMaxPrice) {
                    bidMaxPrice = order.price;
                    bidMaxQty   = order.qty;
                    askCageRefPx = order.price;
                    askWaitingForCage = (mktSubType == MarketSubType::SZSE_STK_GEM);
                }
            } else {
                if (order.price > bidCageRefPx &&
                    (bidCageUpperExMinQty == 0 || order.price < bidCageUpperExMinPrice)) {
                    bidCageUpperExMinPrice = order.price;
                    bidCageUpperExMinQty   = order.qty;
                }
            }
        }
        if (!outOfCage) {
            BidWeightSize  += order.qty;
            BidWeightValue += static_cast<int64_t>(order.price) * order.qty;
        }
    } else if (order.side == Side::ASK) {
        auto it = askLevelTree.find(order.price);
        if (it != askLevelTree.end()) {
            it->second.qty += order.qty;
            if (order.price == askMinPrice)
                askMinQty += order.qty;
            if (askCageLowerExMaxQty && order.price == askCageLowerExMaxPrice)
                askCageLowerExMaxQty += order.qty;
        } else {
            askLevelTree[order.price] = LevelNode(order.price, order.qty);
            if (!outOfCage) {
                if (askMinQty == 0 || order.price < askMinPrice) {
                    askMinPrice = order.price;
                    askMinQty   = order.qty;
                    bidCageRefPx = order.price;
                    bidWaitingForCage = (mktSubType == MarketSubType::SZSE_STK_GEM);
                }
            } else {
                if (order.price < askCageRefPx &&
                    (askCageLowerExMaxQty == 0 || order.price > askCageLowerExMaxPrice)) {
                    askCageLowerExMaxPrice = order.price;
                    askCageLowerExMaxQty   = order.qty;
                }
            }
        }
        if (!outOfCage) {
            // 开盘集合竞价期间，超过昨收N倍的委托不参与统计
            if (tradingPhase == TPM::OpenCall && order.price > mktInfo.PrevClosePx * CYB_ORDER_ENVALUE_MAX_RATE) {
                AskWeightSizeEx  += order.qty;
                AskWeightValueEx += static_cast<int64_t>(order.price) * order.qty;
            } else {
                AskWeightSize  += order.qty;
                AskWeightValue += static_cast<int64_t>(order.price) * order.qty;
            }
        }
    }
}
