#include "axob.h"

// =====================================================================
//  委托处理：onMsg(order) -> onOrder -> onLimitOrder -> insertOrder
//  对应 Python axob.py L470-508, L655-828
// =====================================================================

// ---- 消息入口：逐笔委托 ----
void AXOB::onMsg(const AxsbeOrder& msg) {
    // [v2.7] 构造未指定标的时，由首条消息自动识别 securityID/secSrc
    autoInitFromMsg(msg.secSrc, msg.securityID);
    // [v2优化] securityID 不匹配是罕见情况（文件只含单个股票时不会发生）
    if (UNLIKELY(msg.securityID != SecurityID)) return;
    if (UNLIKELY(isStaleData(msg.TransactTime))) return;

    // CYB 进入收盘集合竞价，敞开价格笼子
    if (cageType == CageType::CYB &&
        tradingPhase == TPM::PMTrading &&
        msg.tradingPhaseMarket() == TPM::CloseCall) {
        openCage();
    }

    useTimestamp(msg.TransactTime);

    // [v2优化] 波动性中断期间不更新交易阶段（罕见路径）
    if (LIKELY(tradingPhase != TPM::VolatilityBreaking)) {
        tradingPhase = msg.tradingPhaseMarket();
    }

    // 进入连续竞价时激活价格笼子 (隐藏集合竞价存量笼子外订单)
    if (UNLIKELY(cageType == CageType::CYB && !cageSessionActive && tradingPhase == TPM::AMTrading)) {
        activateCage();
        cageSessionActive = true;
    }

    // 上交所: 撤单在逐笔委托流中 ('D' 记录, OrderNo 回链原订单)
    if (msg.secSrc == SecurityIDSource_SSE && msg.OrdType == 'D') {
        ObCancel c;
        c.applSeqNum   = msg.OrderNo;
        c.qty          = qtySnap2Inter(msg.OrderQty, msg.secSrc);
        c.price        = msg.Price * SSE_PRICE_MUL;   // 原始 ×10^3 → 内部 ×10^6
        c.side         = msg.isBuy() ? Side::BID : Side::ASK;
        c.TransactTime = msg.TransactTime;
        onCancel(c);
        msgNb++;
        return;
    }

    onOrder(msg);
    msgNb++;
}

// ---- 提取字段，处理市价/本方最优，调用 onLimitOrder ----
void AXOB::onOrder(const AxsbeOrder& rawOrder) {
    ObOrder order(rawOrder, instType);

    // [v2优化] 限价单是最常见类型（~99%），优先判断
    if (LIKELY(order.type == OrdType::LIMIT)) {
        // 限价单：直接进入 onLimitOrder
    } else if (UNLIKELY(order.type == OrdType::MARKET)) {
        // 市价单：后续由成交消息确定价格
    } else if (order.type == OrdType::SIDE) {
        // 本方最优 -> 转限价单（罕见）
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
// [v2优化] holdingOrder 从 unique_ptr 改为栈对象，holdingNb 改为 hasHoldingOrder_
void AXOB::onLimitOrder(ObOrder& order) {
    // 深市市价单 (price=0/超涨跌停价): 按"对手方最优价格申报"成交, 不进入簿。
    // 由 holdingOrder_ 路径承接: 首个成交消息回链 (Bid/OfferApplSeqNum 命中)
    // 时削减对手方档位; 残余自动撤销。参照实现同此语义 (不 updatePriceMap,
    // 簿削减由成交消息回链完成)。
    // [史] 曾加"直接丢弃"guard 消除 0 价幽灵档, 但破坏连续竞价簿削减
    // (000001 65 帧全等失败); 根因是流序 (UNION ALL 分支内排序) 使成交
    // 迟到, 市价单永不回链而滞留在簿 — 全局排序修复后 guard 不再需要。
    if (tradingPhase == TPM::OpenCall || tradingPhase == TPM::CloseCall) {
        // 集合竞价期间：直接插入
        if (tradingPhase == TPM::CloseCall && hasHoldingOrder_) {
            insertOrder(holdingOrder_);
            hasHoldingOrder_ = false;
        }

        // 创业板上市头5日超出范围则丢弃（罕见）
        if (UNLIKELY(tradingPhase == TPM::CloseCall && mktInfo.UpLimitPx == PRICE_MAXIMUM &&
            (order.price > cybMatchUpper(LastPx) || order.price < cybMatchLower(LastPx)))) {
            // 跳过
        } else {
            insertOrder(order);
            bidWaitingForCage = false;
            askWaitingForCage = false;
        }

        genSnap();
    } else {
        // 连续竞价（热路径）
        if (hasHoldingOrder_) {
            // 先把此前缓存的订单插入LOB
            if (UNLIKELY(holdingOrder_.type == OrdType::MARKET && !holdingOrder_.traded)) {
                fprintf(stderr, "%06d MARKET order not followed by trade!\n", SecurityID);
            }
            insertOrder(holdingOrder_);
            hasHoldingOrder_ = false;
            useTimestamp(holdingOrder_.TransactTime);
            genSnap();
            useTimestamp(order.TransactTime);
        }

        // 上交所: 'A' 记录必为挂单 (未成交单或已成交单的剩余), 直接入簿。
        // UA5803 规范: 可成交委托先发成交、再发剩余新增 (或不发), 不存在
        // "可成交的 'A' 记录", 因此深交所的持仓等待逻辑不适用于上交所。
        // 上交所无价格笼子规则, 不做笼子判断。
        if (secSrc == SecurityIDSource_SSE && order.type == OrdType::LIMIT) {
            insertOrder(order);
            genSnap();
            return;
        }

        // CYB 价格笼子判断（仅创业板）
        if (cageType == CageType::CYB && order.type == OrdType::LIMIT &&
            ((order.side == Side::BID && order.price > cybCageUpper(bidCageRefPx)) ||
             (order.side == Side::ASK && order.price < cybCageLower(askCageRefPx)))) {
            insertOrder(order, true);
            genSnap();
        } else if (UNLIKELY(tradingPhase == TPM::VolatilityBreaking)) {
            insertOrder(order);
            genSnap();
        } else {
            // 市价单或可成交限价单 -> 缓存
            // [v2优化] 市价单是罕见情况
            if (UNLIKELY(order.type == OrdType::MARKET)) {
                holdingOrder_ = order;
                hasHoldingOrder_ = true;
            } else if ((order.side == Side::BID && order.price >= askMinPrice && askMinQty > 0) ||
                       (order.side == Side::ASK && order.price <= bidMaxPrice && bidMaxQty > 0)) {
                // 可成交限价单：缓存，等待后续成交消息
                holdingOrder_ = order;
                hasHoldingOrder_ = true;
                bidWaitingForCage = false;
                askWaitingForCage = false;
            } else {
                // 不可成交限价单：直接插入订单簿
                insertOrder(order);
                if (cageType == CageType::CYB) {
                    enterCage();
                }
                genSnap();
            }
        }
    }
}

// ---- 订单入列 ----
// [v2优化] 使用 insertOrderMap 替代直接 operator[]，支持内存池分配
void AXOB::insertOrder(const ObOrder& order, bool outOfCage) {
    insertOrderMap(order.applSeqNum, order);

    if (order.side == Side::BID) {
        auto* node = bidLevelBook.find(order.price);
        if (node) {
            node->qty += order.qty;
            if (order.price == bidMaxPrice)
                bidMaxQty += order.qty;
            if (bidCageUpperExMinQty && order.price == bidCageUpperExMinPrice)
                bidCageUpperExMinQty += order.qty;
        } else {
            bidLevelBook.insert(order.price, order.qty);
            if (!outOfCage) {
                if (bidMaxQty == 0 || order.price > bidMaxPrice) {
                    bidMaxPrice = order.price;
                    bidMaxQty   = order.qty;
                    askCageRefPx = order.price;
                    askWaitingForCage = (cageType == CageType::CYB);
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
            BidWeightValue += order.price * order.qty;
        }
    } else if (order.side == Side::ASK) {
        auto* node = askLevelBook.find(order.price);
        if (node) {
            node->qty += order.qty;
            if (order.price == askMinPrice)
                askMinQty += order.qty;
            if (askCageLowerExMaxQty && order.price == askCageLowerExMaxPrice)
                askCageLowerExMaxQty += order.qty;
        } else {
            askLevelBook.insert(order.price, order.qty);
            if (!outOfCage) {
                if (askMinQty == 0 || order.price < askMinPrice) {
                    askMinPrice = order.price;
                    askMinQty   = order.qty;
                    bidCageRefPx = order.price;
                    bidWaitingForCage = (cageType == CageType::CYB);
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
                AskWeightValueEx += order.price * order.qty;
            } else {
                AskWeightSize  += order.qty;
                AskWeightValue += order.price * order.qty;
            }
        }
    }
}
