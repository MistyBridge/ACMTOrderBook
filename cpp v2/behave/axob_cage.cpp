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
        for (int i = 0; i < bidLevelBook.count; i++) {
            int32_t p = bidLevelBook.levels[i].price;
            if (p > bidMaxPrice && (bidCageUpperExMinQty == 0 || p < bidCageUpperExMinPrice)) {
                if (p > bidCageRefPx) {
                    bidCageUpperExMinPrice = p;
                    bidCageUpperExMinQty   = bidLevelBook.levels[i].qty;
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
        for (int i = askLevelBook.count - 1; i >= 0; i--) {
            int32_t p = askLevelBook.levels[i].price;
            if (p < askMinPrice && (askCageLowerExMaxQty == 0 || p > askCageLowerExMaxPrice)) {
                if (p < askCageRefPx) {
                    askCageLowerExMaxPrice = p;
                    askCageLowerExMaxQty   = askLevelBook.levels[i].qty;
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
                askWaitingForCage = (mktSubType == MarketSubType::SZSE_STK_GEM);

                bidCageUpperExMinQty = 0;
                for (int i = 0; i < bidLevelBook.count; i++) {
                    if (bidLevelBook.levels[i].price > bidCageUpperExMinPrice) {
                        bidCageUpperExMinPrice = bidLevelBook.levels[i].price;
                        bidCageUpperExMinQty   = bidLevelBook.levels[i].qty;
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
                bidWaitingForCage = (mktSubType == MarketSubType::SZSE_STK_GEM);

                askCageLowerExMaxQty = 0;
                for (int i = askLevelBook.count - 1; i >= 0; i--) {
                    if (askLevelBook.levels[i].price < askCageLowerExMaxPrice) {
                        askCageLowerExMaxPrice = askLevelBook.levels[i].price;
                        askCageLowerExMaxQty   = askLevelBook.levels[i].qty;
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
