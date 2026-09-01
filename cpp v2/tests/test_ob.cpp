#include <gtest/gtest.h>
#include "behave/axob.h"
#include "behave/ob_types.h"
#include "tool/axsbe_base.h"
#include <cstdint>

// ---- 精度换算 (axsbe_base.h) ----
TEST(ObTypes, PrecisionConversion) {
    // qty: 深市原生 ×100 (2 位小数), SZSE_QTY_MUL=1 (恒等)
    EXPECT_EQ(qtySnap2Inter(100, SecurityIDSource_SZSE), 100 * SZSE_QTY_MUL);
    // 沪市原生 ×1000 (3 位小数), SSE_QTY_MUL=1 (恒等)
    EXPECT_EQ(qtySnap2Inter(100, SecurityIDSource_SSE), 100 * SSE_QTY_MUL);
    // 反向
    EXPECT_EQ(qtyInter2Snap(100 * SZSE_QTY_MUL, SecurityIDSource_SZSE), 100);
    // 金额: 内部金额 == 交易所金额精度 (÷1)
    EXPECT_EQ(amtInter2Snap(100000, SecurityIDSource_SZSE), 100000 / SZSE_AMT_DIV);
}

TEST(ObTypes, FormatPriceInterToSnap) {
    // 内部价格统一 ×10^6 (官方快照), fmt 输出与内部同尺度 → 恒等。
    EXPECT_EQ(fmtPriceInter2Snap(105400, InstrumentType::STOCK, SecurityIDSource_SZSE), 105400);
    EXPECT_EQ(fmtPriceInter2Snap(10540, InstrumentType::STOCK, SecurityIDSource_SSE), 10540);
}

TEST(ObTypes, MarketSubTypeBySecurityId) {
    EXPECT_EQ(marketSubType(SecurityIDSource_SZSE, 300001), MarketSubType::SZSE_STK_GEM);
    EXPECT_EQ(marketSubType(SecurityIDSource_SZSE, 000001), MarketSubType::SZSE_STK_MB);
    EXPECT_EQ(marketSubType(SecurityIDSource_SZSE, 2001), MarketSubType::SZSE_STK_SME);
    EXPECT_EQ(marketSubType(SecurityIDSource_SSE, 600000), MarketSubType::SSE);
}

TEST(ObTypes, ObOrderConstruction) {
    AxsbeOrder raw;
    raw.secSrc    = SecurityIDSource_SZSE;
    raw.securityID = 300001;
    raw.ApplSeqNum = 1;
    raw.Price      = 105400;  // raw ×10^4 => 10.54
    raw.OrderQty   = 100;     // raw ×100  => 1.00
    raw.Side       = '1';     // buy
    raw.OrdType    = '2';     // limit
    ObOrder o(raw, InstrumentType::STOCK);
    EXPECT_EQ(o.price, 105400 * SZSE_PRICE_MUL);  // 1054000
    EXPECT_EQ(o.qty, 100 * SZSE_QTY_MUL);         // 100000
    EXPECT_EQ(o.side, Side::BID);
    EXPECT_EQ(o.type, OrdType::LIMIT);
}

TEST(ObTypes, ObExecConstruction) {
    AxsbeExe raw;
    raw.secSrc          = SecurityIDSource_SZSE;
    raw.LastPx          = 105400;
    raw.LastQty         = 100;
    raw.BidApplSeqNum   = 1;
    raw.OfferApplSeqNum = 2;
    raw.ExecType        = 'F';
    ObExec e(raw, InstrumentType::STOCK);
    EXPECT_EQ(e.LastPx, 105400 * SZSE_PRICE_MUL);
    EXPECT_EQ(e.LastQty, 100 * SZSE_QTY_MUL);
    EXPECT_TRUE(raw.isTrade());
}

// ---- AXOB 重建冒烟：一买一卖一成交 ----
TEST(AXOB, RebuildBasicBidAskTrade) {
    AXOB axob(300001, SecurityIDSource_SZSE, InstrumentType::STOCK);
    axob.mktInfo.PrevClosePx = 10000000;  // 10.00 (内部 ×10^6)

    auto mkOrder = [&](uint64_t seq, int64_t px, int64_t qty, uint8_t side) {
        AxsbeOrder o;
        o.secSrc = SecurityIDSource_SZSE;
        o.securityID = 300001;
        o.ApplSeqNum = seq;
        o.Price = px;      // raw ×10^4
        o.OrderQty = qty;  // raw ×100
        o.Side = side;
        o.OrdType = '2';
        o.TransactTime = 20220422100000000ULL;  // 10:00 -> AMTrading
        return o;
    };

    // 买一 10.00 × 1000 (raw 100000, 1000)
    axob.onMsg(mkOrder(1, 100000, 1000, '1'));
    // 卖一 10.01 × 500 (raw 100100, 500)
    axob.onMsg(mkOrder(2, 100100, 500, '2'));

    EXPECT_EQ(axob.bidMaxPrice, 100000 * SZSE_PRICE_MUL);
    EXPECT_EQ(axob.askMinPrice, 100100 * SZSE_PRICE_MUL);
    EXPECT_EQ(axob.bidMaxQty, 1000 * SZSE_QTY_MUL);
    EXPECT_EQ(axob.askMinQty, 500 * SZSE_QTY_MUL);

    // 成交：买入 100 股，成交价 10.00
    AxsbeExe ex;
    ex.secSrc = SecurityIDSource_SZSE;
    ex.securityID = 300001;
    ex.BidApplSeqNum = 1;
    ex.OfferApplSeqNum = 2;
    ex.LastPx = 100000;
    ex.LastQty = 100;
    ex.ExecType = 'F';
    ex.TransactTime = 20220422100000000ULL;
    axob.onMsg(ex);

    EXPECT_EQ(axob.NumTrades, 1);
    EXPECT_EQ(axob.LastPx, 100000 * SZSE_PRICE_MUL);
    EXPECT_EQ(axob.bidMaxQty, (1000 - 100) * SZSE_QTY_MUL);
    EXPECT_EQ(axob.askMinQty, (500 - 100) * SZSE_QTY_MUL);
}
