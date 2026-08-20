#!/usr/bin/env python3
# verify_fixes.py — 上一轮低匹配天在修复后的结果对比
# 用法: python verify_fixes.py <old_csv> <new_csv>
import csv, sys

FIELDS = ['mkt','date','inst','engine','status','total','fullExact','statsOnly',
          'mismatch','avgLvl','call_total','call_fullExact',
          'bar_seconds','bar_mismatch','bar_vol_mismatch']

def load(path):
    rows = {}
    with open(path, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f, fieldnames=FIELDS):
            if not r['mkt'] or r['mkt'].startswith('#'):
                continue
            for k in ['total','fullExact','mismatch','bar_mismatch']:
                try: r[k] = int(r[k] or 0)
                except ValueError: r[k] = 0
            try: r['avgLvl'] = float(r['avgLvl'] or 0.0)
            except ValueError: r['avgLvl'] = 0.0
            rows[(r['mkt'], r['date'], r['inst'])] = r
    return rows

def main():
    if len(sys.argv) < 3:
        print("usage: verify_fixes.py <old_csv> <new_csv>")
        return 1
    old, new = load(sys.argv[1]), load(sys.argv[2])

    # 上一轮低匹配天: 全等率 < 99%
    low = [(k, v) for k, v in old.items()
           if v['total'] > 0 and v['fullExact'] / v['total'] < 0.99]
    low.sort(key=lambda kv: kv[1]['fullExact'] / kv[1]['total'])

    print(f"{'标的-日':32s} {'修复前全等率':>10s} {'修复后全等率':>10s} {'修复前档位':>8s} {'修复后档位':>8s}  变化")
    fixed = worse = 0
    for k, o in low:
        n = new.get(k)
        if n is None:
            print(f"{k[0]} {k[1]} {k[2]}  (新结果缺失)")
            continue
        or_ = o['fullExact'] / o['total'] * 100
        nr_ = n['fullExact'] / n['total'] * 100
        tag = ''
        if nr_ > 98.5: fixed += 1; tag = 'FIXED'
        elif nr_ < or_ - 0.5: worse += 1; tag = 'WORSE'
        print(f"{k[0]} {k[1]} {k[2]:30s} {or_:9.2f}% {nr_:9.2f}% "
              f"{o['avgLvl']:7.2f} {n['avgLvl']:7.2f}  {tag}")
    print(f"\n低匹配天总数: {len(low)}, 修复(>98.5%): {fixed}, 仍差: {len(low)-fixed-worse}, 更差: {worse}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
