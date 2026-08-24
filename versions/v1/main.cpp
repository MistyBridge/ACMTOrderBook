#include <cstdio>
#include <chrono>
#include <cstdint>
#include <vector>
#include <algorithm>
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif
#include "tool/msg_util.h"
#include "behave/axob.h"

// 引擎基准统一口径: 单调时钟 (Windows 用 QPC; 其它平台 steady_clock)
static inline uint64_t now_ns() {
#ifdef _WIN32
    static uint64_t freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        freq = f.QuadPart > 0 ? f.QuadPart : 1;
    }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(freq));
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
#endif
}

int main(int argc, char* argv[]) {
    const char* dataFile = (argc > 1) ? argv[1] : "../data/20220422/AX_sbe_szse_000001.log";
    printf("Reading: %s\n", dataFile);
    fflush(stdout);

    AxsbeFileReader reader(dataFile);
    if (!reader.hasNext()) {
        printf("ERROR: file not found or empty\n");
        return 1;
    }
    printf("File opened OK\n");
    fflush(stdout);

    AXOB axob(1, SecurityIDSource_SZSE, InstrumentType::STOCK);
    int orderCnt = 0, exeCnt = 0, snapCnt = 0, totalCnt = 0;
    int nextReport = 0;
    const int reportInterval = 234;   // ~233875 / 1000

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<uint64_t> latNs;
    latNs.reserve(300000);

    while (reader.hasNext()) {
        AxsbeOrder order;
        AxsbeExe   exe;
        AxsbeSnapStock snap;
        int type = reader.next(order, exe, snap);

        uint64_t t0m = now_ns();
        if (isOrdType(type)) {
            axob.onMsg(order);
            orderCnt++;
        } else if (isExeType(type)) {
            axob.onMsg(exe);
            exeCnt++;
        } else if (isSnapType(type)) {
            axob.onMsg(snap);
            snapCnt++;
        }
        latNs.push_back(now_ns() - t0m);
        totalCnt++;
        if (totalCnt >= nextReport) {
            printf("  processed %d msgs...\n", totalCnt);
            fflush(stdout);
            nextReport += reportInterval;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    printf("\n=== Results ===\n");
    printf("Total: %d msgs (order=%d exe=%d snap=%d)\n", totalCnt, orderCnt, exeCnt, snapCnt);
    printf("Time:  %.3f s (%.0f msg/s)\n", elapsed, totalCnt / elapsed);
    if (!latNs.empty()) {
        std::vector<uint64_t> s = latNs;
        std::sort(s.begin(), s.end());
        size_t n = s.size();
        auto q = [&](double p) -> uint64_t {
            size_t idx = static_cast<size_t>((n - 1) * p / 100.0);
            return s[idx];
        };
        uint64_t p50 = q(50), p99 = q(99), pmax = s.back();
        printf("Latency: p50=%.1fus p99=%.1fus pmax=%.1fus (L1 单事件onMsg, 逐条全量, n=%zu)\n",
               p50/1000.0, p99/1000.0, pmax/1000.0, n);
    }
    printf("\nOrderBook State:\n%s\n", axob.toString().c_str());

    auto [askLevels, bidLevels] = axob.getLevels(5);
    printf("\n--- 5 Level OrderBook ---\n");
    for (int i = 4; i >= 0; i--) {
        auto it = askLevels.find(i);
        if (it != askLevels.end() && it->second.qty > 0)
            printf("  Ask[%d]  %d * %d\n", i, it->second.price, it->second.qty);
    }
    printf("  -----\n");
    for (int i = 0; i < 5; i++) {
        auto it = bidLevels.find(i);
        if (it != bidLevels.end() && it->second.qty > 0)
            printf("  Bid[%d]  %d * %d\n", i, it->second.price, it->second.qty);
    }
    fflush(stdout);
    return 0;
}
