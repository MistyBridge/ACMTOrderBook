#include "axob.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>   // std::llabs (内部 ×10^6 定点差值需 64 位绝对值)
#include <map>

static int32_t fmtPx(int64_t price, InstrumentType instType, SecurityIDSource src) {
    return fmtPriceInter2Snap(price, instType, src);
}

// 快照主流价格 (×10^6, 官方) → 内部 ×10^6 (同尺度, ×1)。
static inline int64_t snapPxToInter(int64_t rawPx, SecurityIDSource src) {
    if (src == SecurityIDSource_SZSE) return rawPx * SZSE_SNAP_PRICE_MUL;
    if (src == SecurityIDSource_SSE)  return rawPx * SSE_SNAP_PRICE_MUL;
    return rawPx;
}

// 消息入口
void AXOB::onMsg(const AxsbeSnapStock& msg) {
    autoInitFromMsg(msg.secSrc, msg.securityID);
    if (msg.securityID != SecurityID) return;
    if (UNLIKELY(isStaleData(msg.TransactTime))) return;
    ensureSnap();  // [v2.5] 验证前确保快照最新
    // 套B: 市场快照镜像 (最近一帧原样保留, 秒级更新, 零维护成本)
    if (snapFallbackEnabled) {
        receivedSnapshot = msg;
        hasReceivedSnapshot = true;
        evalSnapRoute(msg);
        onSnap(msg);   // 常量初始化/收盘价补充/波动性中断仍照常 (不触碰簿)
        msgNb++;
        return;
    }
    onSnap(msg);
    msgNb++;
}

void AXOB::onMsg(AXSignal signal) {
    ensureSnap();  // [v2.5] 阶段切换前确保快照最新
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

// 快照常量初始化 (首帧快照; 兜底重置时同样调用)
void AXOB::initMktInfoFromSnap(const AxsbeSnapStock& snap) {
    if (mktInfo.ChannelNo != 0) return;
    mktInfo.ChannelNo    = snap.ChannelNo;
    mktInfo.UpLimitPx    = snap.UpLimitPx;
    mktInfo.DnLimitPx    = snap.DnLimitPx;

    // 快照 PrevClosePx 在数据里是 ×10^4 (与主流价格 ×10^6 不同), 故深市需 ×100 升到 ×10^6。
    mktInfo.PrevClosePx  = (secSrc == SecurityIDSource_SZSE)
                               ? snap.PrevClosePx * SZSE_PRECLOSE_MUL
                               : snapPxToInter(snap.PrevClosePx, secSrc);
    bidCageRefPx = mktInfo.PrevClosePx;
    askCageRefPx = mktInfo.PrevClosePx;

    mktInfo.UpLimitPrice = snapPxToInter(snap.UpLimitPx, secSrc);
    mktInfo.DnLimitPrice = snapPxToInter(snap.DnLimitPx, secSrc);
    mktInfo.YYMMDD       = snap.TransactTime / SZSE_TICK_CUT;
}

// HHMMSSsss (十进制编码) → 当日内毫秒 (真实时间差比较用; 编码差跨秒边界会失真)
static inline int64_t hmsmsToDayMs(uint64_t t) {
    uint64_t hmsms = t % 1000000000ULL;   // HHMMSSsss
    uint64_t sss = hmsms % 1000;
    uint64_t hms = hmsms / 1000;          // HHMMSS
    uint64_t ss = hms % 100; hms /= 100;
    uint64_t mi = hms % 100; hms /= 100;
    uint64_t hh = hms;
    return (int64_t)((hh * 3600 + mi * 60 + ss) * 1000 + sss);
}

// 快照兜底路由 (双套+路由): 套A=重建簿(永不被覆盖), 套B=快照镜像, 输出按条件切换
void AXOB::evalSnapRoute(const AxsbeSnapStock& snap) {
    auto ph = snap.tradingPhaseMarket();

    if (snapRoute == SnapRoute::REBUILT) {
        // 进入快照模式:
        // ① 竞价时段强制 (开盘撮合后至连续竞价前 / 收盘竞价): 簿由快照接管
        if (ph == TPM::PreTradingBreaking || ph == TPM::CloseCall) {
            snapRoute = SnapRoute::SNAPSHOT;
            health.snapRouteAdopt++;
            fprintf(stderr, "SNAP-ROUTE -> SNAPSHOT (竞价时段) t=%llu\n",
                    (unsigned long long)snap.TransactTime);
            return;
        }
        // ② 滞后检测 (仅连续竞价时段: 午休/盘后快照仍推, 不能作为断流信号):
        //    快照时间戳超过最后逐笔时间戳 + 500ms 阈值 → 逐笔流断流/滞后
        //    (正常时快照流比逐笔流早约 1 秒; 厂商反向抖动 ±1-2 笔 <200ms, 阈值过滤误触)
        if ((ph == TPM::AMTrading || ph == TPM::PMTrading) &&
            lastIncTransactTime != 0 &&
            hmsmsToDayMs(snap.TransactTime) > hmsmsToDayMs(lastIncTransactTime) + 500) {
            snapRoute = SnapRoute::SNAPSHOT;
            health.snapRouteAdopt++;
            fprintf(stderr, "SNAP-ROUTE -> SNAPSHOT (逐笔滞后) t=%llu lastInc=%llu\n",
                    (unsigned long long)snap.TransactTime,
                    (unsigned long long)lastIncTransactTime);
            return;
        }
        // ③ 连续竞价交叉簿异常 (竞价阶段买卖交叉是常态, 仅连续竞价判定)
        if ((ph == TPM::AMTrading || ph == TPM::PMTrading) &&
            bidMaxQty > 0 && askMinQty > 0 && bidMaxPrice >= askMinPrice) {
            snapRoute = SnapRoute::SNAPSHOT;
            health.snapRouteAdopt++;
            fprintf(stderr, "SNAP-ROUTE -> SNAPSHOT (交叉簿) t=%llu\n",
                    (unsigned long long)snap.TransactTime);
            return;
        }
    } else {
        // 自愈检测: 套A 与套B 重新对齐 (num_trades 窗口 + 十档全等) → 切回重建簿
        // 丢单场景套A 永远追不上快照笔数 → 停留快照模式 (保真但不假装恢复)。
        if (ph == TPM::PreTradingBreaking || ph == TPM::CloseCall) return;  // 竞价时段不切回
        AxsbeSnapStock a = genTradingSnap(false, 10);
        bool statsAligned = a.NumTrades >= snap.NumTrades &&
                            a.NumTrades <= snap.NumTrades + 2;
        bool lvlsAligned = true;
        for (int i = 0; i < 10 && lvlsAligned; i++) {
            if (!(a.bid[i] == snap.bid[i]) || !(a.ask[i] == snap.ask[i])) lvlsAligned = false;
        }
        if (statsAligned && lvlsAligned) {
            snapRoute = SnapRoute::REBUILT;
            fprintf(stderr, "SNAP-ROUTE -> REBUILT t=%llu\n",
                    (unsigned long long)snap.TransactTime);
        }
    }
}

// 路由感知输出: 快照模式返回套B (市场快照镜像), 否则返回套A (重建簿全量)
AxsbeSnapStock AXOB::currentBook(int showLevelNb) {
    if (snapFallbackEnabled && snapRoute == SnapRoute::SNAPSHOT && hasReceivedSnapshot) {
        return receivedSnapshot;   // 套B: 市场快照镜像 (秒级, 值拷贝开销极小)
    }
    if (tradingPhase == TPM::OpenCall || tradingPhase == TPM::CloseCall)
        return genCallSnap(showLevelNb);
    return genTradingSnap(false, showLevelNb);
}

// 快照验证
void AXOB::onSnap(const AxsbeSnapStock& snap) {
    if (snap.tradingPhaseSecurity() != TPI::Normal) return;

    initMktInfoFromSnap(snap);

    // 收盘价补充 (官方收盘价来自快照, 换算为内部 ×10^6)
    if (tradingPhase == TPM::Ending && !closePxReady) {
        LastPx = snapPxToInter(snap.LastPx, secSrc);
        closePxReady = true;
        genSnap();
    }

    // 波动性中断
    if (snap.tradingPhaseMarket() == TPM::VolatilityBreaking && tradingPhase != TPM::VolatilityBreaking) {
        volatilityBreakingEndTick = 0;
        tradingPhase = TPM::VolatilityBreaking;
        genSnap();
    }

    // [v2.5] onSnap 修改了状态（closePxReady/tradingPhase），需要立即重建快照
    ensureSnap();
}

// [v2.5优化] genSnap 改为延迟标记（~2ns），不立即重建
// 真正的重建由 ensureSnap() 完成
void AXOB::genSnap() {
    snapNeedsUpdate_ = true;
}

// [v2.5] 确保快照最新 — 仅在需要时完整重建（~225ns）
void AXOB::ensureSnap() {
    if (!snapNeedsUpdate_) return;
    snapNeedsUpdate_ = false;

    if (tradingPhase < TPM::OpenCall || tradingPhase > TPM::Ending) return;

    AxsbeSnapStock snap;
    snap.secSrc = secSrc;
    snap.securityID = SecurityID;

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
    lastSnap = snap;
}

// [v2.5] 增量更新统计字段（~5ns，不触发完整重建）
void AXOB::updateSnapStats() {
    lastSnap.LastPx = fmtPx(LastPx, instType, secSrc);
    lastSnap.NumTrades = NumTrades;
    lastSnap.TotalVolumeTrade = qtyInter2Snap(TotalVolumeTrade, secSrc);
    lastSnap.TotalValueTrade = amtInter2Snap(TotalValueTrade, secSrc);
    int32_t h = fmtPx(HighPx, instType, secSrc);
    int32_t l = fmtPx(LowPx, instType, secSrc);
    if (h > lastSnap.HighPx) lastSnap.HighPx = h;
    if (l < lastSnap.LowPx)  lastSnap.LowPx  = l;
}

// 集合竞价快照 — 虚拟撮合算法
AxsbeSnapStock AXOB::genCallSnap(int showLevelNb) {
    int64_t _bid_p = bidMaxPrice;
    int64_t _bid_q = bidMaxQty;
    int64_t _ask_p = askMinPrice;
    int64_t _ask_q = askMinQty;

    // 初始撮合价
    int64_t price = 0;      // 内部 ×10^6
    if (_bid_q == 0 && _ask_q == 0)      price = 0;
    else if (_bid_q == 0)                 price = _ask_p;
    else if (_ask_q == 0)                 price = _bid_p;

    int64_t volumeTrade = 0, bidQty = 0, askQty = 0;
    int64_t refPx = (NumTrades == 0) ? mktInfo.PrevClosePx : LastPx;

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
                // 必须用 std::llabs: 内部 ×10^6 下差值远超 int32,
                // 无限定的 abs() 可能选中 int 重载而截断高位。
                else if (std::llabs(_bid_p - refPx) < std::llabs(_ask_p - refPx)) price = _bid_p;
                else price = _ask_p;
            }

            if (bidQty == 0) {
                if (askQty != 0) price = _ask_p;
                _bid_q = 0;
                // 降序找第一个更低的档 (模式无关)
                bidLevelBook.rfor_each([&](const LevelNode& l) {
                    if (l.price < _bid_p && _bid_q == 0) { _bid_p = l.price; _bid_q = l.qty; }
                });
            }
            if (askQty == 0) {
                if (bidQty != 0) price = _bid_p;
                _ask_q = 0;
                // 升序找第一个更高的档 (模式无关)
                askLevelBook.for_each([&](const LevelNode& l) {
                    if (l.price > _ask_p && _ask_q == 0) { _ask_p = l.price; _ask_q = l.qty; }
                });
            }
        } else {
            // 无交叉，修正成交价
            // 无交叉时向对手价靠一个最小价位 (A股最小价位 = 0.01 元)。
            // 注意: 原实现写字面量 1 (绑定旧 ×10^2 精度), 内部统一 ×10^6 后
            // 必须用 tick1Cent, 否则只挪错误的价位, 虚拟撮合价偏离。
            if (askQty == 0 && bidQty == 0) {
                if (_ask_q && price >= _ask_p) {
                    if (_bid_p + tick1Cent(secSrc) < _ask_p) price = _ask_p - tick1Cent(secSrc);
                    else {
                        if (_ask_q <= _bid_q) { price = _ask_p; askQty = _ask_q; }
                        else { price = _bid_p; bidQty = _bid_q; }
                    }
                } else if (_bid_q && price <= _bid_p) {
                    if (_ask_p > _bid_p + tick1Cent(secSrc)) price = _bid_p + tick1Cent(secSrc);
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
        // 量: 内部 ×10^6 → 快照原始精度
        snap.bid[0] = PriceLevel(snapPrice, qtyInter2Snap(volumeTrade, secSrc));
        snap.ask[0] = PriceLevel(snapPrice, qtyInter2Snap(volumeTrade, secSrc));
        snap.bid[1] = PriceLevel(0, qtyInter2Snap(bidQty, secSrc));
        snap.ask[1] = PriceLevel(0, qtyInter2Snap(askQty, secSrc));
        for (int i = 2; i < showLevelNb; i++) { snap.bid[i] = PriceLevel(0,0); snap.ask[i] = PriceLevel(0,0); }
    }

    snap.NumTrades        = NumTrades;
    // 内部 ×10^6 → 快照原始精度 (量/额 均为原生, 恒等 ÷1)
    snap.TotalVolumeTrade = qtyInter2Snap(TotalVolumeTrade, secSrc);
    snap.TotalValueTrade  = amtInter2Snap(TotalValueTrade, secSrc);
    // 集合竞价快照的 LastPx 为虚拟撮合价 (交易所约定; 无法撮合时为 0)。
    // 注: 部分历史数据源(如 2022 年厂商数据)竞价期 last 恒为 0, 属数据源差异。
    snap.LastPx  = (volumeTrade > 0) ? snapPrice : 0;
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

    // 买方档位（从大到小，模式无关遍历）
    int lv = 0;
    if (!isVolBreaking) {
        bidLevelBook.rfor_each([&](const LevelNode& n) {
            if (lv >= levelNb) return;
            if (bidCageUpperExMinQty == 0 || n.price < bidCageUpperExMinPrice) {
                snap.bid[lv++] = PriceLevel(fmtPx(n.price, instType, secSrc),
                                            qtyInter2Snap(n.qty, secSrc));
            }
        });
    }
    for (int i = lv; i < levelNb; i++) snap.bid[i] = PriceLevel(0, 0);

    // 卖方档位（从小到大，模式无关遍历）
    lv = 0;
    if (!isVolBreaking) {
        askLevelBook.for_each([&](const LevelNode& n) {
            if (lv >= levelNb) return;
            if (askCageLowerExMaxQty == 0 || n.price > askCageLowerExMaxPrice) {
                snap.ask[lv++] = PriceLevel(fmtPx(n.price, instType, secSrc),
                                            qtyInter2Snap(n.qty, secSrc));
            }
        });
    }
    for (int i = lv; i < levelNb; i++) snap.ask[i] = PriceLevel(0, 0);

    setSnapFixParam(snap);
    snap.NumTrades        = NumTrades;
    // 内部 ×10^6 → 快照原始精度 (量/额 均为原生, 恒等 ÷1)
    snap.TotalVolumeTrade = qtyInter2Snap(TotalVolumeTrade, secSrc);
    snap.TotalValueTrade  = amtInter2Snap(TotalValueTrade, secSrc);
    snap.LastPx = fmtPx(LastPx, instType, secSrc);
    snap.HighPx = fmtPx(HighPx, instType, secSrc);
    snap.LowPx  = fmtPx(LowPx,  instType, secSrc);
    snap.OpenPx = fmtPx(OpenPx, instType, secSrc);

    if (isVolBreaking) {
        snap.BidWeightPx = 0; snap.BidWeightSize = 0;
        snap.AskWeightPx = 0; snap.AskWeightSize = 0;
    } else {
        // 加权均价 = Σ(px×qty)/Σqty, 四舍五入 (×2 +1 >>1)。
        // 内部精度对齐交易所原生, Σ(px×qty) 乘积精度 ×10^6, 对境内真实簿远低于 int64 上限,
        // 故用 int64 无需 __int128; 商回落到内部价量级后交给 fmtPx 换算。
        if (BidWeightSize != 0) {
            int64_t wpx = ((BidWeightValue << 1) / BidWeightSize + 1) >> 1;
            snap.BidWeightPx = fmtPx(wpx, instType, secSrc);
        } else snap.BidWeightPx = 0;
        snap.BidWeightSize = qtyInter2Snap(BidWeightSize, secSrc);

        if (AskWeightSize != 0) {
            int64_t wpx = ((AskWeightValue << 1) / AskWeightSize + 1) >> 1;
            snap.AskWeightPx = fmtPx(wpx, instType, secSrc);
        } else snap.AskWeightPx = 0;
        snap.AskWeightSize = qtyInter2Snap(AskWeightSize, secSrc);
    }

    setSnapTimestamp(snap);
    snap.updateTradingPhaseCode(tradingPhase, TPI::Normal);
    return snap;
}
