# -*- coding: utf-8 -*-
import logging
import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tool.axsbe_base import SecurityIDSource_SZSE, TPM, INSTRUMENT_TYPE
from tool.msg_util import axsbe_file
from behave.axob import AXOB

logger = logging.getLogger('main')
logger.setLevel(logging.INFO)
fh = logging.FileHandler('log/run_000001_test.log', mode='w')
fh.setLevel(logging.INFO)
sh = logging.StreamHandler()
sh.setLevel(logging.WARNING)
formatter = logging.Formatter('%(name)s - %(levelname)s - %(message)s')
fh.setFormatter(formatter)
sh.setFormatter(formatter)
logger.addHandler(fh)
logger.addHandler(sh)

date = 20220422
instrument = 1
# 使用绝对路径
md_file = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data', str(date), f'AX_sbe_szse_{instrument:06d}.log')

print(f"=== 订单簿重建测试 ===")
print(f"数据文件: {md_file}")
print(f"股票代码: {instrument:06d}")
print(f"文件存在: {os.path.exists(md_file)}")
print()

axob = AXOB(instrument, SecurityIDSource_SZSE, INSTRUMENT_TYPE.STOCK)

n = 0
t_start = time.time()
for msg in axsbe_file(md_file):
    axob.onMsg(msg)
    n += 1
    if n % 50000 == 0:
        elapsed = time.time() - t_start
        print(f"  已处理 {n} 条消息, 耗时 {elapsed:.1f}s, order_map={axob.order_map_size}, bid={axob.bid_level_tree_size}, ask={axob.ask_level_tree_size}")

elapsed = time.time() - t_start
print(f"\n=== 处理完成 ===")
print(f"总消息数: {n}")
print(f"总耗时: {elapsed:.2f} 秒")
print(f"吞吐量: {n/elapsed:.0f} 条/秒")
print(f"\n=== 订单簿状态 ===")
print(f"成交笔数: {axob.NumTrades}")
print(f"总成交量: {axob.TotalVolumeTrade}")
print(f"总成交额: {axob.TotalValueTrade}")
print(f"最新价: {axob.LastPx/100:.2f}")
print(f"开盘价: {axob.OpenPx/100:.2f}")
print(f"最高价: {axob.HighPx/100:.2f}")
print(f"最低价: {axob.LowPx/100:.2f}")
print(f"order_map大小: {axob.order_map_size}")
print(f"买方档位数: {axob.bid_level_tree_size}")
print(f"卖方档位数: {axob.ask_level_tree_size}")
print(f"\n=== 性能指标 ===")
print(f"order_map峰值: {axob.pf_order_map_maxSize}")
print(f"level_tree峰值: {axob.pf_level_tree_maxSize}")
print(f"bid_level_tree峰值: {axob.pf_bid_level_tree_maxSize}")
print(f"ask_level_tree峰值: {axob.pf_ask_level_tree_maxSize}")

print(f"\n=== 快照验证 ===")
print(f"重建快照缓存数: {sum(len(v) for v in axob.rebuilt_snaps.values())}")
print(f"未匹配市场快照数: {sum(len(v) for v in axob.market_snaps.values())}")

ok = axob.are_you_ok()
if ok:
    print("\n*** 验证通过! 所有交易所快照均已匹配 ***")
else:
    print("\n*** 验证失败! 存在未匹配的交易所快照 ***")

print(f"\n=== 当前订单簿 (前5档) ===")
ask_levels = sorted(axob.ask_level_tree.items(), key=lambda x: x[0], reverse=True)
bid_levels = sorted(axob.bid_level_tree.items(), key=lambda x: x[0], reverse=True)
print("  ASK (卖方):")
for i, (p, l) in enumerate(ask_levels[:5]):
    m = " <-- best" if p == axob.ask_min_level_price else ""
    print(f"    {p/100:.2f}  qty={l.qty}{m}")
print("  ---spread---")
print("  BID (买方):")
for i, (p, l) in enumerate(bid_levels[:5]):
    m = " <-- best" if p == axob.bid_max_level_price else ""
    print(f"    {p/100:.2f}  qty={l.qty}{m}")
