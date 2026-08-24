#include <cstdio>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cstdint>
#include "tool/msg_util.h"
#include "behave/axob.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

// 统一口径 (引擎基准): 单调时钟 + L1 单事件 onMsg 延迟 + T2 引擎纯处理吞吐 (剔除 I/O)。
// Windows 用 QPC (高分辨率单调时钟), 其它平台用 steady_clock。
using Clock = std::chrono::steady_clock;
inline uint64_t now_ns() {
#ifdef _WIN32
    static const uint64_t qpcFreq = []() -> uint64_t {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? static_cast<uint64_t>(f.QuadPart) : 1;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(
        static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(qpcFreq));
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
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

    // L1 延迟: 逐条全量计时 (与 Linux 基准口径一致), 记录每条真实消息的 onMsg 处理耗时 (ns)
    std::vector<uint64_t> latNs;
    uint64_t sampledComputeNs = 0;
    uint64_t samplingCount = 0;

    auto t0 = Clock::now();

    while (reader.hasNext()) {
        AxsbeOrder order;
        AxsbeExe   exe;
        AxsbeSnapStock snap;
        int type = reader.next(order, exe, snap);

        bool isOrd = isOrdType(type);
        bool isExe = isExeType(type);
        bool isSnp = isSnapType(type);
        bool isReal = isOrd || isExe || isSnp;

        uint64_t t0msg = isReal ? now_ns() : 0;   // 逐条全量计时 (与 Linux 基准一致)

        if (isOrd) {
            axob.onMsg(order);
            orderCnt++;
        } else if (isExe) {
            axob.onMsg(exe);
            exeCnt++;
        } else if (isSnp) {
            axob.onMsg(snap);
            snapCnt++;
        }

        if (isReal) {
            uint64_t d = now_ns() - t0msg;
            latNs.push_back(d);
            sampledComputeNs += d;
            samplingCount++;
        }

        totalCnt++;
        if (totalCnt >= nextReport) {
            printf("  processed %d msgs...\n", totalCnt);
            fflush(stdout);
            nextReport += reportInterval;
        }
    }

    auto t1 = Clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    // 百分位 (nearest-index floor(p*(N-1)))
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, pmax = 0;
    if (!latNs.empty()) {
        std::sort(latNs.begin(), latNs.end());
        auto pct = [&](double p) { return (double)latNs[(size_t)(p * (latNs.size() - 1))]; };
        p50  = pct(0.50);
        p90  = pct(0.90);
        p99  = pct(0.99);
        p999 = pct(0.999);
        pmax = pct(1.0);
    }

    double throughputT1 = totalCnt / elapsed;                                    // 系统端到端 (参考)
    double throughputT2 = (samplingCount && sampledComputeNs)
                              ? (double)samplingCount * 1e9 / (double)sampledComputeNs
                              : 0.0;                                             // 引擎纯处理 (主指标)

    printf("\n=== Results ===\n");
    printf("Total: %d msgs (order=%d exe=%d snap=%d)\n", totalCnt, orderCnt, exeCnt, snapCnt);
    // Time 行保持 dashboard.py 兼容格式 (T1 系统端到端); 引擎口径另起一行
    printf("Time:  %.3f s (%.0f msg/s)\n", elapsed, throughputT1);
    printf("Throughput(engine): %.0f msg/s (引擎纯处理 T2, 剔除I/O)\n", throughputT2);
    // Latency 行保持 dashboard.py 兼容 (p50/p99/p99.9/pmax), 值为 L1 单事件 onMsg 耗时
    printf("Latency: p50=%.1fus p99=%.1fus p99.9=%.1fus pmax=%.1fus (L1 单事件onMsg, 逐条全量, n=%llu/%d)\n",
           p50 / 1000.0, p99 / 1000.0, p999 / 1000.0,
           pmax / 1000.0, (unsigned long long)samplingCount, totalCnt);
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
