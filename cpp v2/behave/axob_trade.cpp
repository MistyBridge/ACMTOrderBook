#include "axob.h"
#include <cstdio>
#include <cassert>

void AXOB::onMsg(const AxsbeExe& msg) {
    if (UNLIKELY(msg.securityID != SecurityID)) return;
    if (UNLIKELY(isStaleData(msg.TransactTime))) return;
    // [v2优化] CYB 收盘集合竞价进入是罕见路径
    if (UNLIKELY(cageType == CageType::CYB && tradingPhase == TPM::PMTrading && msg.tradingPhaseMarket() == TPM::CloseCall)) {
        openCage();
    }
    useTimestamp(msg.TransactTime);
    // orderMap 懒清理 (生产开关, 每 65536 条消息一次)
    if (UNLIKELY(orderMapCleanupEnabled && (msgNb & 0xFFFF) == 0)) {
        cleanupOrderMap(msg.TransactTime);
    }
    // 快照模式自愈检测: 逐笔推进时每 32 笔试一次退出 (对齐窗口稍纵即逝,
    // 仅靠 3 秒一帧的快照到达时机检测会错过)
    if (snapFallbackEnabled && snapRoute == SnapRoute::SNAPSHOT &&
        (msgNb & 31) == 0 && hasReceivedSnapshot) {
        evalSnapRoute(receivedSnapshot);
    }
    if (LIKELY(tradingPhase != TPM::VolatilityBreaking)) {
        tradingPhase = msg.tradingPhaseMarket();
    }
    // 进入连续竞价时激活价格笼子 (隐藏集合竞价存量笼子外订单)
    if (UNLIKELY(cageType == CageType::CYB && !cageSessionActive && tradingPhase == TPM::AMTrading)) {
        activateCage();
        cageSessionActive = true;
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
        cancel.qty          = qtySnap2Inter(rawExec.LastQty, rawExec.secSrc);
        cancel.price        = 0;
        cancel.side         = cancelSide;
        cancel.TransactTime = rawExec.TransactTime;
        onCancel(cancel);
    }
}

void AXOB::onTrade(const ObExec& exec) {
    NumTrades++;
    TotalVolumeTrade += exec.LastQty;

    // 成交额累加 (统一定点 ×10^5, 与交易所/品种无关)。
    // 精度对齐交易所原生 (深: 价×10^4/量×10^2, 沪: 价×10^3/量×10^3), 金额 = 价格×数量,
    // 乘积精度 ×10^6 (深/沪皆然), 除以 per-exchange 因子落到交易所金额精度 (深 ÷100 / 沪 ÷10)。
    // 乘积上限 ≈ 单笔金额×10^6, 对境内真实单笔 (≈1e11 元) 仅 1e17, 远低于 int64 上限 9.22e18,
    // 因此用 int64, 无需 __int128。
    TotalValueTrade += amtFromProd(exec.LastPx, exec.LastQty, secSrc);

    LastPx = exec.LastPx;
    // [v2优化] OpenPx==0 仅首笔成交时为真
    if (UNLIKELY(OpenPx == 0)) { OpenPx = exec.LastPx; HighPx = exec.LastPx; LowPx = exec.LastPx; }
    else { if (HighPx < exec.LastPx) HighPx = exec.LastPx; if (LowPx > exec.LastPx) LowPx = exec.LastPx; }

    // 价格笼子基准价 = 最新成交价 (交易所规则)。
    // 之前沿用创业板实现的"跟随簿内最优价"更新路径: 簿顶被隐藏后参考价随之下滑,
    // 进而误藏更多贴近市价的订单 (反馈循环)。创业板笼子同样以此为准。
    if (cageType == CageType::CYB) {
        bidCageRefPx = LastPx;
        askCageRefPx = LastPx;
    }

    // 上交所连续竞价成交削减 (依 UA5803 逐笔合并流语义推导):
    // 被动侧 = 小订单号, 必已挂起故必削减; 主动侧仅当 trade.bizIndex >
    // order.bizIndex (剩余量已挂起) 才削减; 被动侧缺失则整笔跳过簿更新。
    // 竞价/临停时段的成交走下方通用路径 (双侧削减)。
    if (secSrc == SecurityIDSource_SSE &&
        (tradingPhase == TPM::AMTrading || tradingPhase == TPM::PMTrading)) {
        uint64_t bigNo   = (exec.BidApplSeqNum > exec.OfferApplSeqNum)
                               ? exec.BidApplSeqNum : exec.OfferApplSeqNum;
        uint64_t smallNo = (exec.BidApplSeqNum > exec.OfferApplSeqNum)
                               ? exec.OfferApplSeqNum : exec.BidApplSeqNum;
        Side passiveSide = (smallNo == exec.OfferApplSeqNum) ? Side::ASK : Side::BID;
        const ObOrder* smallOrder = getOrder(smallNo);
        if (smallOrder != nullptr) {
            tradeLimit(passiveSide, exec.LastQty, smallNo);
            const ObOrder* bigOrder = getOrder(bigNo);
            if (bigOrder != nullptr && exec.BizIndex > bigOrder->bizIndex) {
                tradeLimit((passiveSide == Side::ASK) ? Side::BID : Side::ASK,
                           exec.LastQty, bigNo);
            }
        }
        updateSnapStats();
        return;
    }

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

    // [v2.5] 增量更新快照统计字段（~5ns），避免完整重建
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
    if (it == orderMap.end()) { health.orderNotFound++; return; }
    ObOrder& order = *(it->second);
    order.qty -= cancel.qty;   // 剩余量递减 (懒清理判定)
    levelDequeue(order.side, order.price, cancel.qty, cancel.applSeqNum);
}

// [v2优化] tradeLimit: orderMap 存 ObOrder* 后需解引用，用 find() 单次查找
void AXOB::tradeLimit(Side side, int64_t qty, uint64_t applSeqNum) {
    auto it = orderMap.find(applSeqNum);
    if (UNLIKELY(it == orderMap.end())) {
        // 上交所 UA5803 规范: 连续竞价阶段主动成交的委托不发新增记录
        // (先发成交, 再发剩余新增; 全部成交则不再发送), 其订单号天然不在簿中,
        // 该侧从未挂起, 直接跳过削减。深交所查不到属真实异常。
        // 计数替代逐笔告警打印: 热路径不做 IO, 异常量由监控采集。
        health.orderNotFound++;
        return;
    }
    ObOrder* o = it->second;
    // 订单剩余量递减: qty==0 = 全成交, 供 orderMap 懒清理判定;
    // 乱序回链安全由 60 秒窗口覆盖。
    o->qty -= qty;
    levelDequeue(side, o->price, qty, applSeqNum);
}

void AXOB::levelDequeue(Side side, int64_t price, int64_t qty, [[maybe_unused]] uint64_t applSeqNum) {
    if (side == Side::BID) {
        auto* node = bidLevelBook.find(price);
        if (!node) return;
        node->qty -= qty;
        if (price == bidMaxPrice) bidMaxQty -= qty;
        // [兜底模式] 快照重建后的簿与订单量脱节, 削减可越界 → 负量档直接清除

        if (bidCageUpperExMinQty == 0 || price < bidCageUpperExMinPrice) {
            BidWeightSize  -= qty;
            BidWeightValue -= price * qty;
        } else if (price == bidCageUpperExMinPrice) {
            bidCageUpperExMinQty -= qty;
            if (bidCageUpperExMinQty == 0) {
                // 升序找第一个更高的档 (模式无关)
                bidLevelBook.for_each([&](const LevelNode& l) {
                    if (l.price > bidCageUpperExMinPrice && bidCageUpperExMinQty == 0) {
                        bidCageUpperExMinPrice = l.price;
                        bidCageUpperExMinQty   = l.qty;
                    }
                });
            }
        }

        if (node->qty < 0) health.negLevelClear++;   // 削减越界保护
        if (node->qty <= 0) {
            bidLevelBook.erase(price);
            if (price == bidMaxPrice) {
                bidMaxQty = 0;
                // erase 已移除原最优价, 取次优 (模式无关: map 模式下 levels[] 为陈旧数据)
                const LevelNode* b = bidLevelBook.bestBid();
                if (b != nullptr) {
                    bidMaxPrice = b->price;
                    bidMaxQty   = b->qty;
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
                AskWeightValueEx -= price * qty;
            } else {
                AskWeightSize  -= qty;
                AskWeightValue -= price * qty;
            }
        } else if (price == askCageLowerExMaxPrice) {
            askCageLowerExMaxQty -= qty;
            if (askCageLowerExMaxQty == 0) {
                // 降序找第一个更低的档 (模式无关)
                askLevelBook.rfor_each([&](const LevelNode& l) {
                    if (l.price < askCageLowerExMaxPrice && askCageLowerExMaxQty == 0) {
                        askCageLowerExMaxPrice = l.price;
                        askCageLowerExMaxQty   = l.qty;
                    }
                });
            }
        }

        if (node->qty < 0) health.negLevelClear++;
        if (node->qty <= 0) {
            askLevelBook.erase(price);
            if (price == askMinPrice) {
                askMinQty = 0;
                // erase 已移除原最优价, 取次优 (模式无关: map 模式下 levels[] 为陈旧数据)
                const LevelNode* a = askLevelBook.bestAsk();
                if (a != nullptr) {
                    askMinPrice = a->price;
                    askMinQty   = a->qty;
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
