# -*- coding: utf-8 -*-
"""
秒K合成结果与 ClickHouse TICK 快照交叉校验

对每个交易日:
  1. 快照锚点校验(连续竞价, 3秒一帧): 到该时刻为止秒K的累计成交量/笔数/最新价
     与 TICK.volume/num_trades/last 对比。厂商快照流与逐笔流时间戳存在 ≤3s 的对齐偏差
     (开盘尤甚), 故按 T..T+3s 窗口取最佳匹配统计。
  2. 日终校验: 秒K合计的 高/低/量/笔/收盘 与 TICK 收盘快照对比。

用法: python py/tool/verify_bars.py <date1> [date2 ...]
"""
import csv
import os
import sys
from datetime import datetime, timedelta

PROJ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_bars(date):
    path = os.path.join(PROJ, "data", date, "bars_sec_000001.csv")
    bars = []
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            bars.append((
                int(row["time_key"]),
                int(row["open"]), int(row["high"]), int(row["low"]), int(row["close"]),
                int(row["volume"]), int(row["turnover"]), int(row["num_trades"]),
            ))
    return bars


def load_ticks(date):
    path = os.path.join(PROJ, "data", date, f"{date}_000001_tick.tsv")
    rows = []
    with open(path, encoding="utf-8") as f:
        for row in csv.reader(f, delimiter="\t"):
            ts = datetime.strptime(row[0], "%Y-%m-%d %H:%M:%S.%f")
            # ts, last, num_trades, volume, high, low
            rows.append((ts, int(row[5]), int(row[6]), int(row[7]), int(row[3]), int(row[4])))
    return rows


def tkey_of(ts):
    return (ts.year * 10**10 + ts.month * 10**8 + ts.day * 10**6
            + ts.hour * 10**4 + ts.minute * 10**2 + ts.second)


def is_continuous(ts):
    hms = ts.hour * 10000 + ts.minute * 100 + ts.second
    return (93000 <= hms < 113000) or (130000 <= hms < 145700)


def main():
    dates = sys.argv[1:] or ["20220422"]
    header = (f"{'日期':<10} {'锚点':>5} | {'量一致':>6} {'价一致':>6} {'笔一致':>6} | "
              f"{'日高差':>10} {'日低差':>10} {'日量差':>12} {'日笔差':>8} {'收盘差':>10}")
    print(header)
    for date in dates:
        bars = load_bars(date)
        ticks = load_ticks(date)

        # 秒K累计前缀
        cum = {}
        vol = trades = 0
        last_px = 0
        high_px = low_px = None
        for (tkey, o, h, l, c, v, tv, n) in bars:
            vol += v
            trades += n
            last_px = c
            if high_px is None or h > high_px:
                high_px = h
            if low_px is None or l < low_px:
                low_px = l
            cum[tkey] = (vol, trades, last_px)

        n_anchor = vol_ok = px_ok = nt_ok = 0
        for ts, last, num_trades, volume, _, _ in ticks:
            if not is_continuous(ts):
                continue
            n_anchor += 1
            # 允许 ±2s 对齐偏差: 实测厂商快照流时间戳比逐笔流约早1秒, 取窗口内最佳匹配
            best_v = best_p = best_n = False
            for k in range(-2, 3):
                tkey = tkey_of(ts + timedelta(seconds=k))
                if tkey not in cum:
                    continue
                cvol, ctr, clast = cum[tkey]
                if cvol == volume:
                    best_v = True
                if clast == last:
                    best_p = True
                if ctr == num_trades:
                    best_n = True
            vol_ok += best_v
            px_ok += best_p
            nt_ok += best_n

        f_ts, f_last, f_nt, f_vol, f_high, f_low = ticks[-1]
        print(f"{date:<10} {n_anchor:>5} | {vol_ok:>6} {px_ok:>6} {nt_ok:>6} | "
              f"{high_px - f_high:>10} {low_px - f_low:>10} {vol - f_vol:>12} "
              f"{trades - f_nt:>8} {last_px - f_last:>10}")

    print("\n单位: 价格差 ×10^4元, 量差 股, 笔差 笔; 日终行取自当日 TICK 最后一帧")
    print("锚点窗口: 快照时刻 T-2s .. T+2s 取最佳匹配(实测厂商快照流时间戳比逐笔流约早1秒)")


if __name__ == "__main__":
    main()
