#include "axob.h"

// =====================================================================
//  创业板价格笼子
//  对应 Python axob.py openCage L602-653, enterCage L955-1011
// =====================================================================

// 阶段切换时，把笼子外的隐藏订单全部放进来
void AXOB::openCage() {
    // 买方：把价格 > bidCageUpperExMinPrice 的隐藏订单加入买方最优
    if (bidCageUpperExMinQty) {
        bidMaxPrice = bidCageUpperExMinPrice;
        bidMaxQty   = bidCageUpperExMinQty;
        BidWeightSize  += bidCageUpperExMinQty;
        BidWeightValue += (int64_t)bidCageUpperExMinPrice * bidCageUpperExMinQty;
        askCageRefPx = bidMaxPrice;

        bidCageUpperExMinQty = 0;
        // 从小到大找下一个更高的隐藏订单
        for (auto& [p, l] : bidLevelTree) {
            if (p > bidMaxPrice && (bidCageUpperExMinQty == 0 || p < bidCageUpperExMinPrice)) {
                if (p > bidCageRefPx) {
                    bidCageUpperExMinPrice = p;
                    bidCageUpperExMinQty   = l.qty;
                }
            }
        }
    }
    bidWaitingForCage = false;

    // 卖方：把价格 < askCageLowerExMaxPrice 的隐藏订单加入卖方最优
    if (askCageLowerExMaxQty) {
        askMinPrice = askCageLowerExMaxPrice;
        askMinQty   = askCageLowerExMaxQty;
        AskWeightSize  += askCageLowerExMaxQty;
        AskWeightValue += (int64_t)askCageLowerExMaxPrice * askCageLowerExMaxQty;
        bidCageRefPx = askMinPrice;

        askCageLowerExMaxQty = 0;
        // 从大到小找下一个更低的隐藏订单
        for (auto rit = askLevelTree.rbegin(); rit != askLevelTree.rend(); ++rit) {
            if (rit->first < askMinPrice && (askCageLowerExMaxQty == 0 || rit->first > askCageLowerExMaxPrice)) {
                if (rit->first < askCageRefPx) {
                    askCageLowerExMaxPrice = rit->first;
                    askCageLowerExMaxQty   = rit->second.qty;
                }
            }
        }
    }
    askWaitingForCage = false;
}

// 连续竞价中，判断隐藏订单是否因基准价变化而进入笼子
void AXOB::enterCage() {
    while (true) {
        // 买方隐藏订单可以进入笼子
        if (bidCageUpperExMinQty && bidCageUpperExMinPrice <= cybCageUpper(bidCageRefPx)) {
            if (askMinQty && bidCageUpperExMinPrice >= askMinPrice) {
                break;  // 可与卖方最优成交，等待
            } else {
                bidMaxPrice = bidCageUpperExMinPrice;
                bidMaxQty   = bidCageUpperExMinQty;
                BidWeightSize  += bidCageUpperExMinQty;
                BidWeightValue += (int64_t)bidCageUpperExMinPrice * bidCageUpperExMinQty;
                askCageRefPx = bidMaxPrice;
                askWaitingForCage = (cageType == CageType::CYB);

                bidCageUpperExMinQty = 0;
                for (auto& [p, l] : bidLevelTree) {
                    if (p > bidCageUpperExMinPrice) {
                        bidCageUpperExMinPrice = p;
                        bidCageUpperExMinQty   = l.qty;
                        break;
                    }
                }
            }
        } else {
            bidWaitingForCage = false;
        }

        // 卖方隐藏订单可以进入笼子
        if (askCageLowerExMaxQty && askCageLowerExMaxPrice >= cybCageLower(askCageRefPx)) {
            if (bidMaxQty && askCageLowerExMaxPrice <= bidMaxPrice) {
                break;  // 可与买方最优成交，等待
            } else {
                askMinPrice = askCageLowerExMaxPrice;
                askMinQty   = askCageLowerExMaxQty;
                AskWeightSize  += askCageLowerExMaxQty;
                AskWeightValue += (int64_t)askCageLowerExMaxPrice * askCageLowerExMaxQty;
                bidCageRefPx = askMinPrice;
                bidWaitingForCage = (cageType == CageType::CYB);

                askCageLowerExMaxQty = 0;
                for (auto rit = askLevelTree.rbegin(); rit != askLevelTree.rend(); ++rit) {
                    if (rit->first < askCageLowerExMaxPrice) {
                        askCageLowerExMaxPrice = rit->first;
                        askCageLowerExMaxQty   = rit->second.qty;
                        break;
                    }
                }
            }
        } else {
            askWaitingForCage = false;
        }

        if (!bidWaitingForCage && !askWaitingForCage) break;
    }
}
