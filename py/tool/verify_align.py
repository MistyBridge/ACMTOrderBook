# -*- coding: utf-8 -*-
"""
严格一致性校验: 两种对齐方式

方式A(时间戳对齐): 快照时刻 T 与秒K(逐笔流)时刻 T+k 对齐, 统计使 volume 精确相等的 k 的分布
方式B(笔数对齐): 快照 num_trades=N 作为逐笔流的精确位置指针, 比较第 N 笔成交处的
  累计成交量与成交价 —— 不受时间戳抖动影响, 是数据一致性的严格判据

用法: python py/tool/verify_align.py <date1> [date2 ...]
"""
import csv
import os
import re
import sys
from collections import Counter
from datetime import datetime, timedelta

PROJ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_execs(date):
    """从 AX-SBE 文件提取逐笔成交序列: [(TransactTime, LastPx, LastQty), ...] 按文件序(seq序)"""
    path = os.path.join(PROJ, "data", date, "AX_sbe_szse_000001.log")
    out = []
    rx_tt = re.compile(r"TransactTime=(\d+)")
    rx_px = re.compile(r"LastPx=(-?\d+)")
    rx_q = re.compile(r"LastQty=(\d+)")
    rx_f = re.compile(r"ExecType=70")
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("//") and "MsgType=191" in line and rx_f.search(line):
                tt = int(rx_tt.search(line).group(1))
                px = int(rx_px.search(line).group(1))
                q = int(rx_q.search(line).group(1))
                out.append((tt, px, q))
    return out


def load_ticks(date):
    path = os.path.join(PROJ, "data", date, f"{date}_000001_tick.tsv")
    rows = []
    with open(path, encoding="utf-8") as f:
        for row in csv.reader(f, delimiter="\t"):
            ts = datetime.strptime(row[0], "%Y-%m-%d %H:%M:%S.%f")
            rows.append((ts, int(row[5]), int(row[6]), int(row[7])))  # ts, last, num_trades, volume
    return rows


def tkey_of(ts):
    return (ts.year * 10**10 + ts.month * 10**8 + ts.day * 10**6
            + ts.hour * 10**4 + ts.minute * 10**2 + ts.second)


def is_continuous(ts):
    hms = ts.hour * 10000 + ts.minute * 100 + ts.second
    return (93000 <= hms < 113000) or (130000 <= hms < 145700)


def main():
    dates = sys.argv[1:] or ["20260713"]
    print(f"{'日期':<10} | 方式B(笔数对齐) {'锚点':>5} {'量全等':>6} {'价全等':>6} | 方式A(时间戳对齐) 量全等占比 错位分布(k:秒)")
    for date in dates:
        execs = load_execs(date)
        ticks = load_ticks(date)
        n = len(execs)
        # 前缀: 第 i 笔(1-based)后的累计量与价
        cum_vol = [0] * (n + 1)
        last_px = [0] * (n + 1)
        v = 0
        for i, (tt, px, q) in enumerate(execs, 1):
            v += q // 100
            cum_vol[i] = v
            last_px[i] = px

        # 秒级累计: tkey -> 该秒末的 (累计量, 笔数)
        sec_cum = {}
        v = 0
        cnt = 0
        for i, (tt, px, q) in enumerate(execs, 1):
            v += q // 100
            cnt += 1
            tkey = (tt // 1000000000) * 1000000 + (tt % 1000000000) // 1000
            sec_cum[tkey] = (v, cnt)

        b_anchor = b_vol = b_px = 0
        a_anchor = a_vol = 0
        skew = Counter()
        for ts, last, nt, volume in ticks:
            if not is_continuous(ts):
                continue
            if nt < 1 or nt > n:
                continue
            # 方式B: 第 N 笔处的累计
            b_anchor += 1
            if cum_vol[nt] == volume:
                b_vol += 1
            if last_px[nt] == last:
                b_px += 1
            # 方式A: 时间戳窗口 ±10s 内找 volume 相等的 k
            a_anchor += 1
            found = None
            for k in range(-10, 11):
                tkey = tkey_of(ts + timedelta(seconds=k))
                if tkey in sec_cum and sec_cum[tkey][0] == volume:
                    found = k
                    break
            if found is not None:
                a_vol += 1
                skew[found] += 1
        b_pct = 100.0 * b_vol / b_anchor
        a_pct = 100.0 * a_vol / a_anchor
        skew_s = " ".join(f"{k}:{skew[k]}" for k in sorted(skew))
        print(f"{date:<10} | {b_anchor:>5} {b_vol:>6} ({b_pct:5.1f}%) {b_px:>6} ({100.0*b_px/b_anchor:5.1f}%) | "
              f"{a_anchor:>5} {a_vol:>5} ({a_pct:5.1f}%)  {skew_s}")
    print("\n方式B: 以快照 num_trades=N 定位逐笔流第 N 笔, 比较累计量与价(时间戳无关)")
    print("方式A: 快照时间戳 vs 逐笔流秒级时间戳, ±10s 窗口内找 volume 相等点, k=逐笔流-快照 的秒差")


if __name__ == "__main__":
    main()
