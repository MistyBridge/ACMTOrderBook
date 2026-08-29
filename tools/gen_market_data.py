#!/usr/bin/env python3
"""
gen_market_data.py — 生成一份可以被 C++ 引擎（MmapFileReader/AxsbeFileReader）
解析的 A 股 L2 逐笔行情 .log 文件。

格式：每行一条 '//Key=Value' 消息；MsgType 192=委托, 191=成交。
字段为深交所(SZSE)口径：
  委托：SecurityIDSource=102 SecurityID=… ApplSeqNum=… Price(×1e4) OrderQty(×100)
        Side(1买/2卖) OrdType(2限价) TransactTime
  成交：BidApplSeqNum OfferApplSeqNum LastPx(×1e4) LastQty(×100) ExecType(F/4) TransactTime

用途：仓库没有真实 .log 数据，用合成流跑 PGO（插桩→产生 profile）与性能测试。
"""
import argparse
import random
import sys


def transact_time(base_day, h, m, s, ms):
    """YYYYMMDDHHMMSSsss 十进制整数 (深沪统一口径, 17 位)。

    base_day=20220422, 上式 HHMMSSsss = (HH*10000+MM*100+SS)*1000 + sss。
    """
    hhmmsssss = (h * 10000 + m * 100 + s) * 1000 + ms
    return base_day * 10 ** 9 + hhmmsssss


def write_order(f, sec, seq, px_raw, qty_raw, side, tt):
    f.write(
        "//MsgType=192 SecurityIDSource=102 SecurityID=%d ApplSeqNum=%d "
        "Price=%d OrderQty=%d Side=%d OrdType=2 TransactTime=%d\n"
        % (sec, seq, px_raw, qty_raw, side, tt)
    )


def write_exe(f, sec, seq, bid, offer, lpx, lqty, tt, exec_type='F'):
    f.write(
        "//MsgType=191 SecurityIDSource=102 SecurityID=%d ApplSeqNum=%d "
        "BidApplSeqNum=%d OfferApplSeqNum=%d LastPx=%d LastQty=%d "
        "ExecType=%c TransactTime=%d\n"
        % (sec, seq, bid, offer, lpx, lqty, exec_type, tt)
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='data_synth.log')
    ap.add_argument('--securities', type=int, default=1)
    ap.add_argument('--msgs', type=int, default=200000)  # 总消息数（约）
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()

    random.seed(args.seed)
    base_day = 20220422
    # 开盘价 10.00 元；深市价格 ×1e4, 数量 ×100；tick=0.01 元
    base_px = 10 * 10000
    px_tick = 100          # 0.01 元 → 100 (×1e4)
    qty_base = 1000        # 10.00 股 → 1000 (×100)

    seq = 0
    tt = transact_time(base_day, 9, 30, 0, 0)
    px = base_px
    buy_counts = 0
    sell_counts = 0
    exe_counts = 0

    with open(args.out, 'w', encoding='ascii') as f:
        for _ in range(args.securities):
            sec = 300001
            # 逐笔消息循环：随机委托/成交混合
            i = 0
            while i < args.msgs:
                r = random.random()
                ms = random.randint(0, 99000)
                tt = transact_time(base_day, 9, 30 + (i // 20000) * 5, 0, ms)
                if r < 0.35:
                    # 买单（价格在买卖中间价下方 / 或可成交）
                    seq += 1
                    px -= random.randint(0, 5) * px_tick
                    write_order(f, sec, seq, px, random.randint(qty_base, 9 * qty_base), 1, tt)
                    buy_counts += 1
                elif r < 0.70:
                    # 卖单
                    seq += 1
                    px += random.randint(0, 5) * px_tick
                    write_order(f, sec, seq, px, random.randint(qty_base, 9 * qty_base), 2, tt)
                    sell_counts += 1
                else:
                    # 成交：回链最近一买一卖
                    seq += 1
                    write_exe(f, sec, seq, seq - 2, seq - 1, px, random.randint(100, 500), tt, 'F')
                    exe_counts += 1
                i += 1
        # 结束信号（可写可不写；引擎读到文件尾即停）
    print("wrote %d -> orders=%d sells=%d execs=%d" % (args.msgs, buy_counts, sell_counts, exe_counts),
          file=sys.stderr)


if __name__ == '__main__':
    main()
