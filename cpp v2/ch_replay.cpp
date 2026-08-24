// ch_replay.cpp — SQL 驱动的订单簿回放+校验工具 (ClickHouse 直连, 无本地文件)
//
// 用法: ch_replay.exe <date:yyyymmdd> <instrument:6位> [host] [port]
//   - 从 ClickHouse 拉取当日 TICK/ORDER/TRANSACTION, 归并后驱动 AXOB 重建订单簿
//   - 市场快照按 num_trades 对齐做等待式校验 (连续竞价比 20 档, 开盘竞价比 genCallSnap,
//     收盘竞价仅比 last)
//   - 输出最终簿状态与校验统计
#include <cstdio>
#include <chrono>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdint>

#include "behave/axob.h"
#include "source/clickhouse_source.h"

using namespace source;

// 统一口径 (引擎基准): 单调时钟 + L1 单事件 onMsg 延迟 + T2 引擎纯处理吞吐 (剔除 I/O)
inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct CheckStat {
    long long total = 0, fullExact = 0, statsOnly = 0, mismatch = 0, levelMatchSum = 0;
    void print(const char* name) const {
        if (!total) return;
        printf("  [%s] total=%lld fullExact=%lld statsOnly=%lld mismatch=%lld avgLvlMatch=%.2f/20\n",
               name, total, fullExact, statsOnly, mismatch,
               (double)levelMatchSum / total);
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: ch_replay <date> <instrument> [host] [port]\n");
        return 1;
    }
    std::string date = argv[1];
    std::string inst = argv[2];
    std::string host = argc > 3 ? argv[3] : "127.0.0.1";
    int port = argc > 4 ? atoi(argv[4]) : 8123;
    // 凭据从环境变量读取 (CH_USER / CH_PASSWORD), 不落盘硬编码
    const char* chUser = getenv("CH_USER");
    const char* chPass = getenv("CH_PASSWORD");
    std::string user = chUser ? chUser : "default";
    std::string pass = chPass ? chPass : "";

    printf("=== ClickHouse 回放: %s %s @%s:%d ===\n", date.c_str(), inst.c_str(), host.c_str(), port);

    ClickHouseSource src(host, port, user, pass);
    auto t0 = std::chrono::steady_clock::now();
    src.load(date, inst, 2);
    auto t1 = std::chrono::steady_clock::now();
    printf("流式查询已建立 (TICK/ORDER/TRANSACTION 三路并发), 耗时 %.3fs\n",
           std::chrono::duration<double>(t1 - t0).count());

    AXOB axob(1, SecurityIDSource_SZSE, InstrumentType::STOCK);

    // 等待式匹配 (与 kline_sec 一致)
    std::map<int64_t, std::vector<AxsbeSnapStock>> pending;
    CheckStat stTrading, stCall;

    auto snapPhase = [](const AxsbeSnapStock& s) -> int {
        auto t = s.tradingPhaseMarket();
        if (t == TPM::OpenCall)  return 1;
        if (t == TPM::CloseCall) return 2;
        return 0;
    };
    auto tryMatch = [&](const AxsbeSnapStock& s, bool finalize) -> bool {
        int ph = snapPhase(s);
        CheckStat& st = (ph == 0) ? stTrading : stCall;
        if (ph == 2) {   // 收盘竞价: 仅比 last
            AxsbeSnapStock gen = axob.genTradingSnap(false, 10);
            bool ok = gen.LastPx == s.LastPx;
            if (ok) { st.total++; st.fullExact++; st.levelMatchSum += 20; }
            else if (finalize) { st.total++; st.mismatch++; }
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
        if (fullOK) { st.total++; st.fullExact++; st.levelMatchSum += 20; return true; }
        if (finalize) {
            st.total++;
            if (statsOK) { st.statsOnly++; st.levelMatchSum += 20 - lvlDiff; }
            else st.mismatch++;
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
            } else ++it;
        }
    };

    auto t2 = std::chrono::steady_clock::now();
    int64_t totalMsgs = 0, tradeCnt = 0;
    std::vector<int64_t> latNs;     // L1 单事件 onMsg 处理耗时 (ns), 逐条全量 (与 Linux 基准一致)
    AxsbeOrder ord; AxsbeExe exe; AxsbeSnapStock snap;
    while (src.hasNext()) {
        int type = src.next(ord, exe, snap);
        bool isReal = (type == MsgType_order || type == MsgType_exe || type == MsgType_snap);
        uint64_t t0m = isReal ? now_ns() : 0;   // 逐条全量计时 (与 Linux 基准一致)
        if (type == MsgType_order) {
            axob.onMsg(ord);
            if (isReal) latNs.push_back(now_ns() - t0m);
            matchPending();
        } else if (type == MsgType_exe) {
            axob.onMsg(exe);
            if (isReal) latNs.push_back(now_ns() - t0m);
            matchPending();
            if (exe.ExecType == 'F') tradeCnt++;
        } else if (type == MsgType_snap) {
            auto tpm = snap.tradingPhaseMarket();
            if (tpm == TPM::AMTrading || tpm == TPM::PMTrading ||
                tpm == TPM::OpenCall || tpm == TPM::CloseCall) {
                pending[snap.NumTrades].push_back(snap);
            }
            axob.onMsg(snap);
            if (isReal) latNs.push_back(now_ns() - t0m);
            matchPending();
        }
        totalMsgs++;
    }
    for (auto& kv : pending)
        for (auto& s : kv.second) tryMatch(s, true);

    auto t3 = std::chrono::steady_clock::now();
    double wallSec = std::chrono::duration<double>(t3 - t2).count();
    printf("拉取完成: 快照 %lld, 委托 %lld, 成交/撤单 %lld, 共 %lld 条\n",
           (long long)src.snapCount(), (long long)src.orderCount(),
           (long long)src.exeCount(), (long long)src.eventCount());

    // L1 延迟 + T2 引擎纯处理吞吐 (逐条全量, 与 Linux 基准一致)
    if (!latNs.empty()) {
        uint64_t sum = 0;
        for (auto v : latNs) sum += (uint64_t)v;
        std::sort(latNs.begin(), latNs.end());
        size_t k = latNs.size();
        auto pct = [&](double p) { return (double)latNs[(size_t)(p * (k - 1))]; };
        double engineTput = sum ? (double)k * 1e9 / (double)sum : 0.0;
        printf("回放完成: %lld 条消息 (成交 %lld), 耗时 %.3fs (%.0f msg/s 端到端含拉取)\n",
               (long long)totalMsgs, (long long)tradeCnt, wallSec,
               wallSec > 0 ? totalMsgs / wallSec : 0.0);
        printf("引擎口径: %.0f msg/s (纯处理 T2, 逐条全量, n=%llu/%lld)\n",
               engineTput, (unsigned long long)k, (long long)totalMsgs);
        printf("Latency(L1 单事件onMsg): p50=%.1fus p99=%.1fus p99.9=%.1fus pmax=%.1fus\n",
               pct(0.50) / 1000.0, pct(0.99) / 1000.0, pct(0.999) / 1000.0, pct(1.0) / 1000.0);
    } else {
        printf("回放完成: %lld 条消息 (成交 %lld), 耗时 %.3fs (%.0f msg/s 端到端含拉取)\n",
               (long long)totalMsgs, (long long)tradeCnt, wallSec,
               wallSec > 0 ? totalMsgs / wallSec : 0.0);
    }
    stTrading.print("连续竞价");
    stCall.print("集合竞价");
    printf("\nOrderBook State:\n%s\n", axob.toString().c_str());
    return 0;
}
