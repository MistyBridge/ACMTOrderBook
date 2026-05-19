# -*- coding: utf-8 -*-
"""
OrderBook Reconstruction Engine - Python Entry Point
Usage: python py/main.py [data_file] [replay_count]
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

def run(data_file, replay_count=1):
    total_order = 0
    total_exe = 0
    total_snap = 0
    total_msgs = 0

    t0 = time.time()

    for replay in range(replay_count):
        if replay_count > 1:
            print(f"  [Replay {replay+1}/{replay_count}]")
        print(f"Reading: {data_file}")
        print(f"File opened OK")

        # 每次重放创建新的 AXOB 实例
        axob = AXOB(1, SecurityIDSource_SZSE, INSTRUMENT_TYPE.STOCK)
        orderCnt = 0
        exeCnt   = 0
        snapCnt  = 0
        totalCnt = 0
        next_report = 0
        report_interval = 234

        replay_t0 = time.time()

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
                # 计算全局消息数和实时速度
                global_cnt = totalCnt + replay * totalCnt
                elapsed_now = time.time() - replay_t0
                speed = int(totalCnt / elapsed_now) if elapsed_now > 0 else 0
                # 格式化为千分位，匹配仪表盘解析格式
                print(f"  {global_cnt:,} msgs  |  {elapsed_now:.1f}s  |  {speed:,} msg/s")
                sys.stdout.flush()
                next_report += report_interval

        total_order += orderCnt
        total_exe += exeCnt
        total_snap += snapCnt
        total_msgs += totalCnt

        # 最后一次重放打印详细状态
        if replay == replay_count - 1:
            print(f"\n=== Results ===")
            print(f"Total: {total_msgs:,} msgs (order={total_order:,} exe={total_exe:,} snap={total_snap:,})")
            elapsed = time.time() - t0
            print(f"Time:  {elapsed:.3f} s ({int(total_msgs/elapsed)} msg/s)")
            print(f"\nOrderBook State:")
            print(f"  orderMap={axob.order_map_size} bidTree={axob.bid_level_tree_size} askTree={axob.ask_level_tree_size}")
            print(f"  bidMax={axob.bid_level_tree_max}  askMin={axob.ask_level_tree_min}")
            print(f"  LastPx={axob.LastPx//100} HighPx={axob.HighPx} LowPx={axob.LowPx} OpenPx={axob.OpenPx}")
            print(f"  NumTrades={axob.NumTrades} TVol={axob.TotalVolumeTrade} TVal={axob.TotalValueTrade}")
            print(f"  tradingPhase={axob.tradingPhaseMarket}")

            ask, bid = axob._getLevels(5)
            print(f"\n--- 5 Level OrderBook ---")
            for i in range(4, -1, -1):
                if i in ask and ask[i].Qty > 0:
                    print(f"  Ask[{i}]  {ask[i].Price} * {ask[i].Qty}")
            print(f"  -----")
            for i in range(5):
                if i in bid and bid[i].Qty > 0:
                    print(f"  Bid[{i}]  {bid[i].Price} * {bid[i].Qty}")

if __name__ == "__main__":
    data_file = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DATA
    replay_count = int(sys.argv[2]) if len(sys.argv) > 2 else 1

    # 限制重放次数范围
    replay_count = max(1, min(replay_count, 1000))

    if not os.path.exists(data_file):
        print(f"ERROR: file not found: {data_file}")
        sys.exit(1)
    run(data_file, replay_count)
