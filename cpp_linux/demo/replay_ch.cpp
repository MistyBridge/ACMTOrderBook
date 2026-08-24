// replay_ch.cpp — 演示: 通过 libacmt_orderbook.so 直连 ClickHouse 回放 (无本地文件)
// 用法: ./replay_ch <date:yyyymmdd> <instrument:6位> [exchange:1=沪 2=深] [host] [port]
#include "acmt_orderbook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <date> <instrument> [exchange] [host] [port]\n", argv[0]);
        return 1;
    }
    const char* date = argv[1];
    const char* inst = argv[2];
    int exchange = argc > 3 ? atoi(argv[3]) : 2;
    const char* host = argc > 4 ? argv[4] : "127.0.0.1";
    int port = argc > 5 ? atoi(argv[5]) : 8123;

    acmt_ob_handle h = acmt_ob_create(inst, exchange);
    if (!h) {
        fprintf(stderr, "acmt_ob_create failed\n");
        return 1;
    }

    printf("=== ClickHouse 回放: %s %s (exchange=%d) @%s:%d ===\n",
           date, inst, exchange, host, port);
    // 可选参数: <mode:bench|fallback|skip> [skipSod] [skipSec]
    //   fallback + skipSod/skipSec = 丢单模拟 + 快照兜底验证
    //   skip     + skipSod/skipSec = 丢单模拟 (兜底关闭, 对照基线)
    bool bench = (argc > 6 && strcmp(argv[6], "bench") == 0);
    bool fb    = (argc > 6 && strcmp(argv[6], "fallback") == 0);
    bool skip  = (argc > 6 && strcmp(argv[6], "skip") == 0);
    int skipSod = ((fb || skip) && argc > 7) ? atoi(argv[7]) : 0;
    int skipSec = ((fb || skip) && argc > 8) ? atoi(argv[8]) : 0;
    // 生产健壮性开关: argv[6]="cleanup" → orderMap 懒清理 + 跨日过滤
    if (argc > 6 && strcmp(argv[6], "cleanup") == 0) {
        acmt_ob_set_order_cleanup(h, 1);
        acmt_ob_set_stale_filter(h, 1);
    }
    // 凭据从环境变量读取 (CH_USER / CH_PASSWORD), 不落盘硬编码
    const char* chUser = getenv("CH_USER");
    const char* chPass = getenv("CH_PASSWORD");
    std::string user = chUser ? chUser : "default";
    std::string pass = chPass ? chPass : "";
    // 引擎基准统一口径: 一律用单调时钟 steady_clock。
    auto t0 = std::chrono::steady_clock::now();
    int64_t n = bench
        ? acmt_ob_replay_ch_bench(h, host, port, user.c_str(), pass.c_str(),
                                  date, inst, exchange)
        : acmt_ob_replay_ch_sim(h, host, port, user.c_str(), pass.c_str(),
                                date, inst, exchange, fb ? 1 : 0, skipSod, skipSec);
    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    if (n < 0) {
        fprintf(stderr, "replay failed\n");
        acmt_ob_destroy(h);
        return 1;
    }
    // 此处为系统端到端吞吐 (含 ClickHouse 拉取/解析/校验)。
    // 引擎纯处理口径 (T2) 与 L1 延迟分布由 .so 在 REPLAYSTAT / LAT (stderr) 行输出,
    // 引擎横向对比请用该行, 勿用此端到端值。
    printf("replay time=%.3fs (%.2fM msg/s 端到端含拉取; 引擎纯处理见 stderr REPLAYSTAT, %s)\n",
           elapsed, n / elapsed / 1e6,
           bench ? "纯回放(快照只喂不校验)"
                 : (fb ? "丢单模拟(快照兜底ON)" : "含拉取+回放+校验"));
    if (bench) {
        acmt_ob_destroy(h);
        return 0;
    }

    // 显示精度: 深交所价格 ×10^4/数量 ×10^2; 上交所 ×10^3/×10^3
    const double pxDiv = (exchange == 1) ? 1000.0 : 10000.0;
    const double qtyDiv = (exchange == 1) ? 1000.0 : 100.0;

    acmt_ob_stat_t st;
    acmt_ob_get_stat(h, &st);
    acmt_snap_t book;
    acmt_ob_get_book(h, &book);

    printf("replayed %lld msgs (orders=%lld trades=%lld)\n",
           (long long)n, (long long)st.order_count, (long long)st.trade_count);
    printf("NumTrades=%lld Volume=%lld(×10^2股) Turnover=%lld(×10^4元)\n",
           (long long)st.num_trades, (long long)st.total_volume_trade,
           (long long)st.total_value_trade);
    // 用 pxDiv 而非硬编码 10000: 沪市原始价精度为 ×10^3
    printf("Last=%.2f Open=%.2f High=%.2f Low=%.2f\n",
           st.last_px / pxDiv, st.open_px / pxDiv,
           st.high_px / pxDiv, st.low_px / pxDiv);
    printf("--- 重建簿 5 档 ---\n");
    for (int i = 4; i >= 0; i--) {
        if (book.ask_price[i] > 0 && book.ask_volume[i] > 0)
            printf("  Ask[%d] %.2f * %lld\n", i, book.ask_price[i] / pxDiv,
                   (long long)(book.ask_volume[i] / qtyDiv));
    }
    printf("  -----\n");
    for (int i = 0; i < 5; i++) {
        if (book.bid_price[i] > 0 && book.bid_volume[i] > 0)
            printf("  Bid[%d] %.2f * %lld\n", i, book.bid_price[i] / pxDiv,
                   (long long)(book.bid_volume[i] / qtyDiv));
    }

    acmt_ob_validation_t v;
    acmt_ob_get_validation(h, &v);
    printf("--- 快照校验 (num_trades 对齐) ---\n");
    printf("  连续竞价: total=%lld fullExact=%lld statsOnly=%lld mismatch=%lld avgLvl=%.2f/20\n",
           (long long)v.trading_total, (long long)v.trading_full_exact,
           (long long)v.trading_stats_only, (long long)v.trading_mismatch,
           v.trading_avg_level_match);
    printf("  集合竞价: total=%lld fullExact=%lld mismatch=%lld\n",
           (long long)v.call_total, (long long)v.call_full_exact,
           (long long)v.call_mismatch);
    printf("  1s聚合: seconds=%lld mismatch=%lld volMismatch=%lld\n",
           (long long)v.bar_seconds, (long long)v.bar_mismatch,
           (long long)v.bar_vol_mismatch);
    printf("AUDIT total=%lld fullExact=%lld statsOnly=%lld mismatch=%lld avgLvl=%.2f call_total=%lld call_fullExact=%lld bar_seconds=%lld bar_mismatch=%lld bar_vol_mismatch=%lld\n",
           (long long)v.trading_total, (long long)v.trading_full_exact,
           (long long)v.trading_stats_only, (long long)v.trading_mismatch,
           v.trading_avg_level_match,
           (long long)v.call_total, (long long)v.call_full_exact,
           (long long)v.bar_seconds, (long long)v.bar_mismatch,
           (long long)v.bar_vol_mismatch);
    acmt_ob_health_t hl;
    acmt_ob_get_health(h, &hl);
    printf("health: orderNotFound=%llu negLevelClear=%llu snapRouteAdopt=%llu cleanupErased=%llu staleFiltered=%llu\n",
           (unsigned long long)hl.order_not_found,
           (unsigned long long)hl.neg_level_clear,
           (unsigned long long)hl.snap_route_adopt,
           (unsigned long long)hl.cleanup_erased,
           (unsigned long long)hl.stale_filtered);
    acmt_ob_destroy(h);
    return 0;
}
