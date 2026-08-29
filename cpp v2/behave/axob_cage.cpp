#include "axob.h"

// =====================================================================
//  价格笼子 (2% 有效竞价范围): 创业板/上交所股票
//  对应 Python axob.py openCage L602-653, enterCage L955-1011
// =====================================================================

// 进入连续竞价时, 隐藏集合竞价期间挂在簿内、超出有效竞价范围的存量订单。
// 参考价取最新成交价 (无成交取昨收)。隐藏档仍在价格簿中, 通过
// bidCageUpperExMinPrice/Qty (买方笼子外最低价) 与 askCageLowerExMaxPrice/Qty
// 标记, 快照生成时过滤; 权重统计同步扣除。
void AXOB::activateCage() {
    if (cageType != CageType::CYB) return;

    int64_t ref = (LastPx > 0) ? LastPx : mktInfo.PrevClosePx;
    if (ref <= 0) return;
    int64_t up = cybCageUpper(ref);
    int64_t lo = cybCageLower(ref);

    // 买方: 隐藏价格 > up 的档 (模式无关遍历)
    bidCageUpperExMinPrice = 0;
    bidCageUpperExMinQty   = 0;
    bidLevelBook.for_each([&](const LevelNode& n) {
        int64_t p = n.price;
        int64_t q = n.qty;
        if (p > up) {
            if (bidCageUpperExMinQty == 0 || p < bidCageUpperExMinPrice) {
                bidCageUpperExMinPrice = p;
                bidCageUpperExMinQty   = q;
            }
            BidWeightSize  -= q;
            BidWeightValue -= p * q;
        }
    });
    // 若最优买档被隐藏, 重定位最优可见买档
    if (bidMaxPrice > up) {
        bidMaxPrice = 0;
        bidMaxQty   = 0;
        bidLevelBook.rfor_each([&](const LevelNode& n) {
            if (n.price <= up) {
                bidMaxPrice = n.price;
                bidMaxQty   = n.qty;
            }
        });
    }

    // 卖方: 隐藏价格 < lo 的档 (模式无关遍历)
    askCageLowerExMaxPrice = 0;
    askCageLowerExMaxQty   = 0;
    askLevelBook.for_each([&](const LevelNode& n) {
        int64_t p = n.price;
        int64_t q = n.qty;
        if (p < lo) {
            if (askCageLowerExMaxQty == 0 || p > askCageLowerExMaxPrice) {
                askCageLowerExMaxPrice = p;
                askCageLowerExMaxQty   = q;
            }
            AskWeightSize  -= q;
            AskWeightValue -= p * q;
        }
    });
    // 若最优卖档被隐藏, 重定位最优可见卖档
    if (askMinPrice && askMinPrice < lo) {
        askMinPrice = 0;
        askMinQty   = 0;
        askLevelBook.for_each([&](const LevelNode& n) {
            if (n.price >= lo && askMinPrice == 0) {
                askMinPrice = n.price;
                askMinQty   = n.qty;
            }
        });
    }

    // 参考价跟随最优可见档
    if (bidMaxPrice) askCageRefPx = bidMaxPrice;
    if (askMinPrice) bidCageRefPx = askMinPrice;
}

// 阶段切换时，把笼子外的隐藏订单全部放进来
void AXOB::openCage() {
    // 买方：把价格 > bidCageUpperExMinPrice 的隐藏订单加入买方最优
    if (bidCageUpperExMinQty) {
        bidMaxPrice = bidCageUpperExMinPrice;
        bidMaxQty   = bidCageUpperExMinQty;
        BidWeightSize  += bidCageUpperExMinQty;
        BidWeightValue += bidCageUpperExMinPrice * bidCageUpperExMinQty;
        askCageRefPx = bidMaxPrice;

        bidCageUpperExMinQty = 0;
        // 从小到大找下一个更高的隐藏订单（模式无关遍历）
        bidLevelBook.for_each([&](const LevelNode& n) {
            int64_t p = n.price;
            if (p > bidMaxPrice && (bidCageUpperExMinQty == 0 || p < bidCageUpperExMinPrice)) {
                if (p > bidCageRefPx) {
                    bidCageUpperExMinPrice = p;
                    bidCageUpperExMinQty   = n.qty;
                }
            }
        });
    }
    bidWaitingForCage = false;

    // 卖方：把价格 < askCageLowerExMaxPrice 的隐藏订单加入卖方最优
    if (askCageLowerExMaxQty) {
        askMinPrice = askCageLowerExMaxPrice;
        askMinQty   = askCageLowerExMaxQty;
        AskWeightSize  += askCageLowerExMaxQty;
        AskWeightValue += askCageLowerExMaxPrice * askCageLowerExMaxQty;
        bidCageRefPx = askMinPrice;

        askCageLowerExMaxQty = 0;
        // 从大到小找下一个更低的隐藏订单（模式无关遍历）
        askLevelBook.rfor_each([&](const LevelNode& n) {
            int64_t p = n.price;
            if (p < askMinPrice && (askCageLowerExMaxQty == 0 || p > askCageLowerExMaxPrice)) {
                if (p < askCageRefPx) {
                    askCageLowerExMaxPrice = p;
                    askCageLowerExMaxQty   = n.qty;
                }
            }
        });
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
                BidWeightValue += bidCageUpperExMinPrice * bidCageUpperExMinQty;
                askCageRefPx = bidMaxPrice;
                askWaitingForCage = (cageType == CageType::CYB);

                bidCageUpperExMinQty = 0;
                bidLevelBook.for_each([&](const LevelNode& n) {
                    if (bidCageUpperExMinQty == 0 && n.price > bidCageUpperExMinPrice) {
                        bidCageUpperExMinPrice = n.price;
                        bidCageUpperExMinQty   = n.qty;
                    }
                });
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
                AskWeightValue += askCageLowerExMaxPrice * askCageLowerExMaxQty;
                bidCageRefPx = askMinPrice;
                bidWaitingForCage = (cageType == CageType::CYB);

                askCageLowerExMaxQty = 0;
                askLevelBook.rfor_each([&](const LevelNode& n) {
                    if (askCageLowerExMaxQty == 0 && n.price < askCageLowerExMaxPrice) {
                        askCageLowerExMaxPrice = n.price;
                        askCageLowerExMaxQty   = n.qty;
                    }
                });
            }
        } else {
            askWaitingForCage = false;
        }

        if (!bidWaitingForCage && !askWaitingForCage) break;
    }
}
