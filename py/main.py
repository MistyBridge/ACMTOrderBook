# -*- coding: utf-8 -*-
"""
OrderBook Reconstruction Engine - Python Entry Point
Usage: python py/main.py [data_file]
"""
import os, sys, time

if getattr(sys, "frozen", False):
    _HERE = os.path.dirname(os.path.abspath(sys.executable))
else:
    _HERE = os.path.dirname(os.path.abspath(__file__))

sys.path.insert(0, _HERE)
from tool.msg_util import axsbe_file
from behave.axob import AXOB
from tool.axsbe_base import SecurityIDSource_SZSE, INSTRUMENT_TYPE

PROJECT_DIR = os.path.dirname(_HERE)
DEFAULT_DATA = os.path.join(PROJECT_DIR, "data", "20220422", "AX_sbe_szse_000001.log")

def run(data_file):
    print(f"Reading: {data_file}")
    print(f"File opened OK")

    axob = AXOB(1, SecurityIDSource_SZSE, INSTRUMENT_TYPE.STOCK)
    orderCnt = 0
    exeCnt   = 0
    snapCnt  = 0
    totalCnt = 0
    next_report = 0
    report_interval = 234

    t0 = time.time()

    for msg in axsbe_file(data_file):
        axob.onMsg(msg)
        totalCnt += 1
        if hasattr(msg, 'ApplSeqNum') and hasattr(msg, 'OrderQty'):
            orderCnt += 1
        elif hasattr(msg, 'ExecType'):
            exeCnt += 1
        else:
            snapCnt += 1
        if totalCnt >= next_report:
            print(f"  processed {totalCnt} msgs...")
            sys.stdout.flush()
            next_report += report_interval

    elapsed = time.time() - t0

    print(f"\n=== Results ===")
    print(f"Total: {totalCnt} msgs (order={orderCnt} exe={exeCnt} snap={snapCnt})")
    print(f"Time:  {elapsed:.3f} s ({int(totalCnt/elapsed)} msg/s)")
    print(f"\nOrderBook State:")
    print(f"  orderMap={axob.order_map_size} bidTree={axob.bid_level_tree_size} askTree={axob.ask_level_tree_size}")
    print(f"  bidMax={axob.bid_level_tree_max}  askMin={axob.ask_level_tree_min}")
    print(f"  LastPx={axob.LastPx} HighPx={axob.HighPx} LowPx={axob.LowPx} OpenPx={axob.OpenPx}")
    print(f"  NumTrades={axob.NumTrades} TVol={axob.TotalVolumeTrade} TVal={axob.TotalValueTrade}")
    print(f"  tradingPhase={axob.tradingPhaseMarket}")

    ask, bid = axob.getLevels(5)
    print(f"\n--- 5 Level OrderBook ---")
    for i in range(4, -1, -1):
        if i in ask and ask[i].qty > 0:
            print(f"  Ask[{i}]  {ask[i].price} * {ask[i].qty}")
    print(f"  -----")
    for i in range(5):
        if i in bid and bid[i].qty > 0:
            print(f"  Bid[{i}]  {bid[i].price} * {bid[i].qty}")

if __name__ == "__main__":
    data_file = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DATA
    if not os.path.exists(data_file):
        print(f"ERROR: file not found: {data_file}")
        sys.exit(1)
    run(data_file)
