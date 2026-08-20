# -*- coding: utf-8 -*-
"""调试: 对比重建快照与市场快照的逐字段差异 (诊断用,非正式代码)
用法: python py/dbg_snap_diff.py [HHMMSS]  默认 093003
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tool.axsbe_base import SecurityIDSource_SZSE, INSTRUMENT_TYPE
from tool.msg_util import axsbe_file, axsbe_snap_stock
from behave.axob import AXOB

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(PROJ, "data", "20220422", "AX_sbe_szse_000001.log")
TARGET = "20220422" + (sys.argv[1] if len(sys.argv) > 1 else "093003") + "000"

axob = AXOB(1, SecurityIDSource_SZSE, INSTRUMENT_TYPE.STOCK)
target_snap = None
n = 0
for msg in axsbe_file(DATA):
    if isinstance(msg, axsbe_snap_stock) and msg.TransactTime == int(TARGET):
        target_snap = msg
        break
    axob.onMsg(msg)
    n += 1

print(f"到达目标快照前已处理 {n} 条消息")
print(f"axob.NumTrades={axob.NumTrades}  LastPx={axob.LastPx}  holding_nb={axob.holding_nb}")

axob.genSnap()
rebuilt = axob.last_snap
if rebuilt is None:
    print("重建快照为空,无法对比")
    sys.exit(1)

print(f"\n市场快照: NumTrades={target_snap.NumTrades}  重建: NumTrades={rebuilt.NumTrades}")
print(f"市场: LastPx={target_snap.LastPx} Vol={target_snap.TotalVolumeTrade} Val={target_snap.TotalValueTrade}")
print(f"重建: LastPx={rebuilt.LastPx} Vol={rebuilt.TotalVolumeTrade} Val={rebuilt.TotalValueTrade}")
print(f"市场: Open={target_snap.OpenPx} High={target_snap.HighPx} Low={target_snap.LowPx}")
print(f"重建: Open={rebuilt.OpenPx} High={rebuilt.HighPx} Low={rebuilt.LowPx}")
print(f"市场: BidW={target_snap.BidWeightPx}/{target_snap.BidWeightSize} AskW={target_snap.AskWeightPx}/{target_snap.AskWeightSize}")
print(f"重建: BidW={rebuilt.BidWeightPx}/{rebuilt.BidWeightSize} AskW={rebuilt.AskWeightPx}/{rebuilt.AskWeightSize}")

print("\n--- 10档对比 (市场 vs 重建) ---")
ok = 0
for i in range(10):
    mb, ma = target_snap.bid[i], target_snap.ask[i]
    rb, ra = rebuilt.bid[i], rebuilt.ask[i]
    if mb == rb:
        ok += 1
    if ma == ra:
        ok += 1
    print(f"B[{i}] {'OK ' if mb==rb else 'DIFF'} 市场 {mb.Price}*{mb.Qty}   重建 {rb.Price}*{rb.Qty}   | A[{i}] {'OK ' if ma==ra else 'DIFF'} 市场 {ma.Price}*{ma.Qty}   重建 {ra.Price}*{ra.Qty}")
print(f"\n匹配档位: {ok}/20")
print(f"is_same 结果: {target_snap.is_same(rebuilt)}")
