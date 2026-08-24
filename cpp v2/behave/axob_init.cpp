#include "axob.h"
#include <cstdio>
#include <algorithm>

// =====================================================================
//  AXOB 构造 + 工具函数
//  对应 Python axob.py: __init__ L377-469, _useTimestamp L1343,
//  _setSnapFixParam L1322, _setSnapTimestamp L1352, _clipSnap L1339,
//  _getLevels L1650, _fmtPrice_inter2snap L1628, __str__ L1777
// =====================================================================

// [v2优化] 构造函数：支持外部传入 MemoryPool，或自行创建
#if USE_MEMORY_POOL
AXOB::AXOB(int securityID, SecurityIDSource src, InstrumentType type,
           axob::core::MemoryPool<ObOrder>* pool)
    : SecurityID(securityID), secSrc(src), instType(type)
{
    // 内存池初始化：外部传入则复用，否则自建
    if (pool) {
        orderPool_ = pool;
        ownPool_ = false;
    } else {
        orderPool_ = new axob::core::MemoryPool<ObOrder>(4096);
        ownPool_ = true;
    }

    // 市场子类型自动识别
    mktSubType = marketSubType(src, securityID);

    // 价格笼子 (2% 有效竞价范围): 仅创业板 (深交所交易规则)。
    // 上交所不设笼子: 远档订单自然沉底不影响前十档, 已实测 24 天
    // 17 天满分/其余 19.6+; 生产场景的兜底由快照路由覆盖。
    cageType = (mktSubType == MarketSubType::SZSE_STK_GEM)
               ? CageType::CYB : CageType::NONE;
}
#else
AXOB::AXOB(int securityID, SecurityIDSource src, InstrumentType type)
    : SecurityID(securityID), secSrc(src), instType(type)
{
    mktSubType = marketSubType(src, securityID);
    cageType = (mktSubType == MarketSubType::SZSE_STK_GEM)
               ? CageType::CYB : CageType::NONE;
}
#endif

// [v2优化] 析构函数：释放 orderMap 中所有堆分配的 ObOrder
AXOB::~AXOB() {
    // 释放 orderMap 中的 ObOrder*
    for (auto& [seq, ptr] : orderMap) {
#if USE_MEMORY_POOL
        if (orderPool_) orderPool_->free(ptr);
        else delete ptr;
#else
        delete ptr;
#endif
    }
    orderMap.clear();

#if USE_MEMORY_POOL
    // 如果自行创建了内存池，释放它
    if (ownPool_ && orderPool_) {
        delete orderPool_;
        orderPool_ = nullptr;
    }
#endif

    // [v2.2优化] lastSnap 已改为栈上对象，无需 delete
}

// ---- 时戳转内部精度 ----
// 时间口径统一: 深沪均使用 YYYYMMDDHHMMSSsss (北京时间) 十进制整数
void AXOB::useTimestamp(uint64_t transactTime) {
    lastIncTransactTime = transactTime;
    currentIncTick = (transactTime / SZSE_TICK_MS_TAIL) %
                     (SZSE_TICK_CUT / SZSE_TICK_MS_TAIL);
    if (currentIncTick >= (1ULL << TIMESTAMP_BIT_SIZE)) {
        fprintf(stderr, "%06d TransactTime=%llu ovf!\n", SecurityID,
                (unsigned long long)transactTime);
    }
}

// ---- 跨日脏数据检测 (生产开关) ----
bool AXOB::isStaleData(uint64_t transactTime) {
    if (!staleDataFilterEnabled) return false;
    int64_t day = (int64_t)(transactTime / 1000000000ULL);   // YYYYMMDD
    if (dayOfData < 0) { dayOfData = day; return false; }
    if (day < dayOfData) { staleFiltered++; return true; }
    return false;
}

// ---- orderMap 懒清理 (生产内存控制) ----
// 触发: 每 65536 条消息一次, round-robin 分段扫描 (约 8 段/轮), 避免集中停顿。
// 删除条件: 订单剩余量 qty==0 (全成交) 且时间戳早于 60 秒安全窗口
// (流内按 seq/biz_index 排序, 乱序回链风险已由排序+窗口双重覆盖)。
void AXOB::cleanupOrderMap(uint64_t nowTransactTime) {
    if (!orderMapCleanupEnabled || orderMap.empty()) return;
    // HHMMSSsss 编码 → 当日真实毫秒 (跨秒边界编码差会失真)
    auto dayMs = [](uint64_t t) -> int64_t {
        uint64_t hmsms = t % 1000000000ULL;
        uint64_t sss = hmsms % 1000;
        uint64_t hms = hmsms / 1000;
        uint64_t ss = hms % 100; hms /= 100;
        uint64_t mi = hms % 100; hms /= 100;
        return (int64_t)(((hms * 3600 + mi * 60 + ss) * 1000) + sss);
    };
    constexpr int64_t SAFE_MS = 60000;
    size_t total = orderMap.size();
    size_t seg = (total / 8) + 1;
    size_t scanned = 0;
    auto it = orderMap.begin();
    while (it != orderMap.end() && cleanupCursor > 0) { ++it; --cleanupCursor; }
    int64_t nowMs = dayMs(nowTransactTime);
    for (; it != orderMap.end() && scanned < seg; ) {
        auto cur = it++;
        ObOrder* o = cur->second;
        if (o->qty == 0 && nowMs - dayMs(o->TransactTime) > SAFE_MS) {
            orderMap.erase(cur);
            health.cleanupErased++;
        }
        scanned++;
    }
    if (it == orderMap.end()) cleanupCursor = 0;
    else cleanupCursor += scanned;
}

// ---- 快照固定参数 ----
void AXOB::setSnapFixParam(AxsbeSnapStock& snap) {
    snap.securityID = SecurityID;
    snap.ChannelNo  = mktInfo.ChannelNo;
    snap.UpLimitPx  = mktInfo.UpLimitPx;
    snap.DnLimitPx  = mktInfo.DnLimitPx;

    // 内部 ×10^5 → 快照原始精度 (统一定点后与品种无关)
    if (secSrc == SecurityIDSource_SZSE)
        snap.PrevClosePx = clipInt32(mktInfo.PrevClosePx / SZSE_PRICE_MUL);
}

// ---- 时戳写入快照 ----
void AXOB::setSnapTimestamp(AxsbeSnapStock& snap) {
    if (secSrc == SecurityIDSource_SZSE) {
        snap.TransactTime = mktInfo.YYMMDD * SZSE_TICK_CUT +
                           (currentIncTick * SZSE_TICK_MS_TAIL);
    } else {
        snap.TransactTime = currentIncTick / 100;
    }
}

// ---- 大数钳位 ----
void AXOB::clipSnap(AxsbeSnapStock& snap) {
    snap.AskWeightPx = clipInt32(static_cast<int64_t>(snap.AskWeightPx));
}

// ---- 内部精度转快照精度 (统一实现见 ob_types.h::fmtPriceInter2Snap) ----
[[maybe_unused]] static int32_t fmtPrice(int64_t price, InstrumentType instType,
                                         SecurityIDSource src) {
    return fmtPriceInter2Snap(price, instType, src);
}

// ---- 取N档买卖盘（返回内部精度，与 v1 一致）----
std::pair<std::map<int64_t,LevelNode>, std::map<int64_t,LevelNode>>
AXOB::getLevels(int levelNb) {
    std::map<int64_t,LevelNode> askResult, bidResult;

    // 卖方：从小到大 (模式无关遍历)
    {
        int idx = 0;
        askLevelBook.for_each([&](const LevelNode& l) {
            if (idx < levelNb) { askResult[idx] = l; idx++; }
        });
        for (; idx < levelNb; idx++) askResult[idx] = LevelNode(0, 0);
    }

    // 买方：从大到小 (模式无关遍历)
    {
        int idx = 0;
        bidLevelBook.rfor_each([&](const LevelNode& l) {
            if (idx < levelNb) { bidResult[idx] = l; idx++; }
        });
        for (; idx < levelNb; idx++) bidResult[idx] = LevelNode(0, 0);
    }

    return {askResult, bidResult};
}

// ---- 打印状态 ----
std::string AXOB::toString() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "%06d tick=%llu msgs=%d\n"
        "  orderMap=%d bidTree=%d askTree=%d\n"
        "  bidMax=%lld*%lld  askMin=%lld*%lld\n"
        "  LastPx=%lld HighPx=%lld LowPx=%lld OpenPx=%lld\n"
        "  NumTrades=%lld TVol=%lld TVal=%lld\n"
        "  tradingPhase=%s",
        SecurityID, (unsigned long long)currentIncTick, msgNb,
        orderMapSize(), bidTreeSize(), askTreeSize(),
        (long long)bidMaxPrice, (long long)bidMaxQty, (long long)askMinPrice, (long long)askMinQty,
        (long long)LastPx, (long long)HighPx, (long long)LowPx, (long long)OpenPx,
        (long long)NumTrades, (long long)TotalVolumeTrade, (long long)TotalValueTrade,
        tpm_str(tradingPhase));
    return buf;
}
