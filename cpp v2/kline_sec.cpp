// kline_sec.cpp — 秒级K线合成 + 订单簿快照校验工具(num_trades 对齐)
//
// 用法: kline_sec.exe <axsbe_file> <out_bars_csv> [start_sec] [end_sec]
//   - 读入 AX-SBE 历史文件, 用 AXOB 重建订单簿
//   - 逐笔成交('F')聚合为秒级 OHLCV, 写入 CSV
//   - 盘口校验: 市场快照按 num_trades 缓存, 引擎处理到相同笔数时生成重建快照并逐档对比
//     (num_trades 是两条流共用的精确位置指针, 不受厂商时间戳错位影响)
//   - 连续竞价(AM/PM)用 genTradingSnap 对比; 集合竞价(OpenCall/CloseCall)用 genCallSnap 对比
//
// 输出 CSV 列(单位):
//   time_key, open, high, low, close, volume, turnover, num_trades
//   time_key : YYYYMMDDHHMMSS
//   open/high/low/close : 价格 ×10^4
//   volume   : 股
//   turnover : 元 ×10^6
//   num_trades: 笔
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <cinttypes>
#include <map>
#include <vector>

#include "behave/axob.h"
#include "tool/msg_util.h"

struct SecBar {
    uint64_t tkey = 0;          // YYYYMMDDHHMMSS
    int64_t open = 0, high = 0, low = 0, close = 0;    // ×10^4
    int64_t volume = 0;         // 股
    int64_t turnover = 0;       // 元 ×10^6
    int32_t numTrades = 0;
    bool active = false;
};

struct SnapCheckStat {
    int64_t total = 0;          // 参与校验的快照数
    int64_t fullExact = 0;      // 20档+统计全部一致
    int64_t statsOnly = 0;      // 统计一致但档位有差
    int64_t mismatch = 0;       // 统计不一致
    int64_t levelMatchSum = 0;  // 匹配档位数之和
    int64_t maxLevelDiff = 0;
    int64_t maxValDiff = 0;     // 最大成交额差(×10^4元, 引擎逐笔取整)
    uint64_t firstBadTkey = 0;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: kline_sec <axsbe_file> <out_csv> [start_sec] [end_sec]\n");
        return 1;
    }
    const char* dataFile = argv[1];
    const char* outFile  = argv[2];
    uint32_t startSec = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;   // 如 93000
    uint32_t endSec   = argc > 4 ? (uint32_t)atoi(argv[4]) : 0;   // 如 150000, 0=不限

    AXOB axob(1, SecurityIDSource_SZSE, InstrumentType::STOCK);
    AxsbeFileReader reader(dataFile);

    FILE* fout = fopen(outFile, "w");
    if (!fout) { fprintf(stderr, "cannot open %s\n", outFile); return 1; }
    fprintf(fout, "time_key,open,high,low,close,volume,turnover,num_trades\n");

    SnapCheckStat stTrading{}, stCall{};
    SecBar bar;
    auto flushBar = [&]() {
        if (!bar.active) return;
        fprintf(fout, "%08llu,%lld,%lld,%lld,%lld,%lld,%lld,%d\n",
                (unsigned long long)bar.tkey,
                (long long)bar.open, (long long)bar.high,
                (long long)bar.low,  (long long)bar.close,
                (long long)bar.volume, (long long)bar.turnover, bar.numTrades);
        bar = SecBar{};
    };

    // 待匹配的市场快照: num_trades -> [snaps]
    std::map<int64_t, std::vector<AxsbeSnapStock>> pending;

    // 匹配策略: 快照按 num_trades 键缓存; 引擎笔数等于键时, 每次事件后都尝试比对,
    // 订单流在窗口内陆续到达, 簿补全的瞬间即可匹配; 笔数越过键仍未匹配 -> 最终分类。
    // 口径: 连续竞价比 20 档+统计; 开盘集合竞价比 genCallSnap 全量;
    //       收盘集合竞价为交易所特殊展示口径, 仅比 last。
    auto snapPhase = [](const AxsbeSnapStock& s) -> int {
        auto t = s.tradingPhaseMarket();
        if (t == TPM::OpenCall)  return 1;   // 开盘集合竞价
        if (t == TPM::CloseCall) return 2;   // 收盘集合竞价
        return 0;                            // 连续竞价
    };
    auto tryMatch = [&](const AxsbeSnapStock& s, bool finalize) -> bool {
        int ph = snapPhase(s);
        SnapCheckStat& st = (ph == 0) ? stTrading : stCall;
        if (ph == 2) {   // 收盘竞价: 仅比 last(最后成交价)
            AxsbeSnapStock gen = axob.genTradingSnap(false, 10);
            bool ok = gen.LastPx == s.LastPx;
            if (ok) { st.total++; st.fullExact++; st.levelMatchSum += 20; }
            else if (finalize) {
                st.total++;
                st.mismatch++;
                if (!st.firstBadTkey)
                    st.firstBadTkey = (s.TransactTime / 1000000000ULL) * 1000000u +
                                      (s.TransactTime % 1000000000ULL) / 1000;
            }
            return ok;
        }
        AxsbeSnapStock gen = (ph == 1) ? axob.genCallSnap(10) : axob.genTradingSnap(false, 10);
        bool statsOK = gen.NumTrades == s.NumTrades &&
                       gen.TotalVolumeTrade == s.TotalVolumeTrade &&
                       gen.LastPx == s.LastPx;
        int64_t lvlDiff = 0;
        for (int k = 0; k < 10; k++) {
            if (!(gen.bid[k] == s.bid[k])) lvlDiff++;
            if (!(gen.ask[k] == s.ask[k])) lvlDiff++;
        }
        bool fullOK = statsOK && lvlDiff == 0;
        if (fullOK) {   // 20 档全等: 计数
            st.total++;
            st.fullExact++;
            st.levelMatchSum += 20;
            return true;
        }
        if (finalize) {
            st.total++;
            if (statsOK) { st.statsOnly++; st.levelMatchSum += 20 - lvlDiff; }
            else {
                st.mismatch++;
                if (!st.firstBadTkey) {
                    st.firstBadTkey = (s.TransactTime / 1000000000ULL) * 1000000u +
                                      (s.TransactTime % 1000000000ULL) / 1000;
                    printf("DBG mismatch t=%llu mkt(nt=%lld vol=%lld last=%d) gen(nt=%lld vol=%lld last=%d) mkt b0=%d*%lld a0=%d*%lld gen b0=%d*%lld a0=%d*%lld\n",
                           (unsigned long long)s.TransactTime,
                           (long long)s.NumTrades, (long long)s.TotalVolumeTrade, s.LastPx,
                           (long long)gen.NumTrades, (long long)gen.TotalVolumeTrade, gen.LastPx,
                           s.bid[0].Price, (long long)s.bid[0].Qty, s.ask[0].Price, (long long)s.ask[0].Qty,
                           gen.bid[0].Price, (long long)gen.bid[0].Qty, gen.ask[0].Price, (long long)gen.ask[0].Qty);
                }
            }
        }
        return false;
    };
    auto matchPending = [&]() {
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->first < axob.NumTrades) {
                for (auto& s : it->second) tryMatch(s, true);
                it = pending.erase(it);
                continue;
            }
            if (it->first == axob.NumTrades) {
                auto& vec = it->second;
                for (size_t i = 0; i < vec.size();) {
                    if (tryMatch(vec[i], false)) vec.erase(vec.begin() + i);
                    else i++;
                }
                if (vec.empty()) it = pending.erase(it);
                else ++it;
            } else {
                ++it;
            }
        }
    };

    auto t0 = std::chrono::high_resolution_clock::now();
    int64_t totalMsgs = 0, tradeCnt = 0, snapCnt = 0;

    AxsbeOrder ord; AxsbeExe exe; AxsbeSnapStock snap;
    while (reader.hasNext()) {
        int type = reader.next(ord, exe, snap);
        totalMsgs++;
        if (type == MsgType_order) {
            axob.onMsg(ord);
            matchPending();  // 订单到达可能补全窗口内的簿
        } else if (type == MsgType_exe) {
            axob.onMsg(exe);
            matchPending();  // 引擎笔数推进, 尝试匹配快照
            if (exe.ExecType == 'F') {  // 只聚合成交
                uint32_t day = (uint32_t)(exe.TransactTime / 1000000000ULL);
                uint32_t ms  = (uint32_t)(exe.TransactTime % 1000000000ULL);
                uint32_t secOfDay = ms / 1000;                 // HHMMSS
                if (secOfDay < startSec) continue;
                if (endSec && secOfDay > endSec) continue;
                uint64_t tkey = (uint64_t)day * 1000000u + secOfDay;  // 防 uint32 乘法溢出
                if (bar.active && bar.tkey != tkey) flushBar();
                if (!bar.active) {
                    bar.active = true;
                    bar.tkey = tkey;
                    bar.open = bar.high = bar.low = bar.close = exe.LastPx;
                } else {
                    if (exe.LastPx > bar.high) bar.high = exe.LastPx;
                    if (exe.LastPx < bar.low)  bar.low  = exe.LastPx;
                    bar.close = exe.LastPx;
                }
                bar.volume   += exe.LastQty / 100;       // LastQty ×100 -> 股
                bar.turnover += exe.LastPx * exe.LastQty; // (×10^4)*(×100) = 元×10^6
                bar.numTrades++;
                tradeCnt++;
            }
        } else if (type == MsgType_snap) {
            snapCnt++;
            // 连续竞价/集合竞价快照按 num_trades 缓存, 交给 matchPending 处理
            auto tpm = snap.tradingPhaseMarket();
            if (tpm == TPM::AMTrading || tpm == TPM::PMTrading ||
                tpm == TPM::OpenCall || tpm == TPM::CloseCall) {
                pending[snap.NumTrades].push_back(snap);
            }
            axob.onMsg(snap);
            matchPending();
        }
    }
    flushBar();

    // 收尾: 未匹配的快照做最终分类
    for (auto& kv : pending) {
        for (auto& s : kv.second) {
            tryMatch(s, true);
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    fclose(fout);

    printf("kline_sec done: %s\n", dataFile);
    printf("  msgs=%lld trades=%lld snaps=%lld time=%.3fs (%.0f msg/s)\n",
           (long long)totalMsgs, (long long)tradeCnt, (long long)snapCnt,
           elapsed, totalMsgs / elapsed);
    auto report = [&](const char* name, const SnapCheckStat& st) {
        if (!st.total) return;
        printf("  [%s] total=%lld fullExact=%lld statsOnly=%lld mismatch=%lld avgLvlMatch=%.2f/20 maxValDiff=%lld firstBadT=%llu\n",
               name, (long long)st.total, (long long)st.fullExact, (long long)st.statsOnly,
               (long long)st.mismatch,
               st.total ? (double)st.levelMatchSum / st.total : 0.0,
               (long long)st.maxValDiff, (unsigned long long)st.firstBadTkey);
    };
    report("连续竞价", stTrading);
    report("集合竞价", stCall);
    return 0;
}
