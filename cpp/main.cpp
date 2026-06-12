#include <cstdio>
#include <chrono>
#include "tool/msg_util.h"
#include "behave/axob.h"

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

    while (reader.hasNext()) {
        AxsbeOrder order;
        AxsbeExe   exe;
        AxsbeSnapStock snap;
        int type = reader.next(order, exe, snap);

        if (type == MsgType_order) {
            axob.onMsg(order);
            orderCnt++;
        } else if (type == MsgType_exe) {
            axob.onMsg(exe);
            exeCnt++;
        } else if (type == MsgType_snap) {
            axob.onMsg(snap);
            snapCnt++;
        }
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
