# -*- coding: utf-8 -*-
"""
从 ClickHouse LEVEL2 库导出单只股票某日数据，转换为 AX-SBE 历史文件格式（见 doc/msgTypes.md）。

用法:
    python py/tool/ch_to_axsbe.py [date] [instrument] [out_file]

    date       : yyyymmdd，如 20220422
    instrument : 6 位代码，如 000001
    out_file   : 输出的 .log 文件（默认 data/<date>/AX_sbe_szse_<instrument>.log）

仅支持深交所 (exchange=2)。输出只写 //Key=Value 注释行，py 与 cpp 的解析器均只读注释行。
"""
import base64
import csv
import io
import os
import sys
import urllib.request
from datetime import datetime

# 连接信息从环境变量读取, 不落盘硬编码
CH_HOST = os.environ.get("CH_HOST", "127.0.0.1")
CH_PORT = os.environ.get("CH_PORT", "8123")
CH_USER = os.environ.get("CH_USER", "default")
CH_PASS = os.environ.get("CH_PASSWORD", "")
CH_URL = "http://%s:%s/" % (CH_HOST, CH_PORT)
CH_AUTH = "Basic " + base64.b64encode(("%s:%s" % (CH_USER, CH_PASS)).encode()).decode()

# ---- AX-SBE 常量（与 tool/axsbe_base.py 一致）----
SID_SZSE = 102
MT_SNAP = 111
MT_ORDER = 192
MT_EXE = 191
MSGLEN_SNAP = 352
MSGLEN_ORDER = 48
MSGLEN_EXE = 64

# 交易阶段 Code0（深交所，见 doc/msgTypes.md）
TP_STARTING, TP_OPENCALL, TP_TRADING, TP_BREAKING, TP_CLOSECALL, TP_ENDING = 0, 1, 2, 3, 4, 5


def ch_query(sql: str) -> str:
    req = urllib.request.Request(CH_URL, data=sql.encode("utf-8"))
    req.add_header("Authorization", CH_AUTH)
    with urllib.request.urlopen(req, timeout=900) as r:
        return r.read().decode("utf-8", "replace")


def tsv_rows(sql: str):
    text = ch_query(sql)
    return list(csv.reader(io.StringIO(text), delimiter="\t"))


def parse_ts(s: str) -> datetime:
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f")


def transact_time(dt: datetime) -> int:
    """YYYYMMDDHHMMSSsss 十进制整数"""
    return (dt.year * 10**13 + dt.month * 10**11 + dt.day * 10**9 + dt.hour * 10**7
            + dt.minute * 10**5 + dt.second * 10**3 + dt.microsecond // 1000)


def trading_phase_code0(dt: datetime) -> int:
    """由时刻推导深交所交易阶段（CH TICK 无交易阶段字段）"""
    hms = dt.hour * 10000000 + dt.minute * 100000 + dt.second * 1000 + dt.microsecond // 1000
    if hms < 91500000:
        return TP_STARTING
    if hms < 92500000:
        return TP_OPENCALL
    if hms < 93000000:
        return TP_BREAKING
    if hms < 113000000:
        return TP_TRADING
    if hms < 130000000:
        return TP_BREAKING
    if hms < 145700000:
        return TP_TRADING
    if hms < 150000000:
        return TP_CLOSECALL
    return TP_ENDING


def side_asz(side: int) -> int:
    """CH side (66='B' 83='S') -> 深交所 '1'买/'2'卖"""
    if side == 66:
        return 49  # '1'
    if side == 83:
        return 50  # '2'
    raise ValueError(f"非法 side={side}")


def ordtype_asz(order_type: int) -> int:
    """CH order_type 首字节 ('0'/'1'/'U') -> AX-SBE OrdType ('2'限价/'1'市价/'U'本方最优)"""
    c = order_type // 256
    if c == 0x30:   # '0' 限价
        return 50   # '2'
    if c == 0x31:   # '1' 市价
        return 49   # '1'
    if c == 0x55:   # 'U' 本方最优
        return 85
    raise ValueError(f"非法 order_type={order_type}")


def exectype_asz(trade_type: int) -> int:
    """CH trade_type: 12355='0C' 撤单，其余为成交"""
    if trade_type == 12355:
        return 52   # '4' 撤销
    return 70       # 'F' 成交


def fetch_export(date: str, instrument: str, tmp_dir: str):
    """从 ClickHouse 拉取三类数据到本地 TSV"""
    ins = f"{instrument}\0\0"
    sql_snap = (
        f"SELECT timestamp, pre_close, open, high, low, last, num_trades, volume, turnover, "
        f"total_bid_vol, total_ask_vol, upper_limit, lower_limit, w_avg_bid_p, w_avg_ask_p, "
        + ",".join(f"bid_p{i}, bid_v{i}, ask_p{i}, ask_v{i}" for i in range(10)) +
        f" FROM LEVEL2.TICK_{date} WHERE instrument='{ins}' AND exchange=2 ORDER BY timestamp"
    )
    sql_order = (
        f"SELECT seq_no, timestamp, price, quantity, side, order_type "
        f"FROM LEVEL2.ORDER_{date} WHERE instrument='{ins}' AND exchange=2 ORDER BY seq_no"
    )
    sql_exe = (
        f"SELECT seq_no, timestamp, price, quantity, side, trade_type, bid_no, ask_no "
        f"FROM LEVEL2.TRANSACTION_{date} WHERE instrument='{ins}' AND exchange=2 ORDER BY seq_no"
    )
    files = {}
    for name, sql in (("tick", sql_snap), ("order", sql_order), ("exe", sql_exe)):
        path = os.path.join(tmp_dir, f"{date}_{instrument}_{name}.tsv")
        with open(path, "w", encoding="utf-8") as f:
            f.write(ch_query(sql))
        files[name] = path
        print(f"  已下载 {name}: {path}")
    return files


def load_snaps(path: str, snap_price_scale: int):
    """TICK TSV -> [(timestamp, comment_line), ...] 按时间序

    snap_price_scale: 快照价格的 10 的幂次
      6 -> ×10^6 (与 doc/msgTypes.md 及 C++ 引擎一致, 默认)
      4 -> ×10^4 (与 py 引擎 _fmtPrice_inter2snap 一致)
    CH 价格本身为 ×10^4, 故 scale=6 时 ×100, scale=4 时 ×1。
    """
    px_mul = 100 if snap_price_scale == 6 else 1
    snaps = []
    with open(path, encoding="utf-8") as f:
        for row in csv.reader(f, delimiter="\t"):
            (ts, pre_close, open_, high, low, last, num_trades, volume, turnover,
             tbv, tav, uplim, dnlim, wavg_b, wavg_a) = row[:15]
            lv = row[15:]
            dt = parse_ts(ts)
            kvs = [
                f"SecurityIDSource={SID_SZSE}", f"MsgType={MT_SNAP}", f"MsgLen={MSGLEN_SNAP}",
                f"SecurityID=1", f"ChannelNo=2013",
                f"TradingPhase={trading_phase_code0(dt)}",
                f"NumTrades={int(num_trades)}",
                f"TotalVolumeTrade={int(volume) * 100}",
                f"TotalValueTrade={int(turnover)}",
                f"PrevClosePx={int(pre_close)}",
                f"LastPx={int(last) * px_mul}",
                f"OpenPx={int(open_) * px_mul}",
                f"HighPx={int(high) * px_mul}",
                f"LowPx={int(low) * px_mul}",
                f"BidWeightPx={int(wavg_b) * px_mul}",
                f"BidWeightSize={int(tbv) * 100}",
                f"AskWeightPx={int(wavg_a) * px_mul}",
                f"AskWeightSize={int(tav) * 100}",
                f"UpLimitPx={int(uplim) * px_mul}",
                f"DnLimitPx={int(dnlim) * px_mul}",
                f"TransactTime={transact_time(dt)}",
            ]
            for i in range(10):
                kvs.append(f"BidLevel[{i}].Price={int(lv[0 + i * 4]) * px_mul}")
                kvs.append(f"BidLevel[{i}].Qty={int(lv[1 + i * 4]) * 100}")
                kvs.append(f"AskLevel[{i}].Price={int(lv[2 + i * 4]) * px_mul}")
                kvs.append(f"AskLevel[{i}].Qty={int(lv[3 + i * 4]) * 100}")
            snaps.append((dt, "//" + " ".join(kvs)))
    return snaps


def load_incs(order_path: str, exe_path: str):
    """ORDER/TRANSACTION TSV -> [(seq_no, timestamp, comment_line), ...] 按 seq 合并排序"""
    incs = []
    with open(order_path, encoding="utf-8") as f:
        for row in csv.reader(f, delimiter="\t"):
            seq, ts, price, qty, side, order_type = row
            dt = parse_ts(ts)
            kvs = [
                f"SecurityIDSource={SID_SZSE}", f"MsgType={MT_ORDER}", f"MsgLen={MSGLEN_ORDER}",
                f"SecurityID=1", f"ChannelNo=2013", f"ApplSeqNum={int(seq)}",
                f"Price={int(price)}", f"OrderQty={int(qty) * 100}",
                f"Side={side_asz(int(side))}", f"OrdType={ordtype_asz(int(order_type))}",
                f"TransactTime={transact_time(dt)}",
            ]
            incs.append((int(seq), dt, "//" + " ".join(kvs)))
    with open(exe_path, encoding="utf-8") as f:
        for row in csv.reader(f, delimiter="\t"):
            seq, ts, price, qty, side, trade_type, bid_no, ask_no = row
            dt = parse_ts(ts)
            is_cancel = int(trade_type) == 12355
            kvs = [
                f"SecurityIDSource={SID_SZSE}", f"MsgType={MT_EXE}", f"MsgLen={MSGLEN_EXE}",
                f"SecurityID=1", f"ChannelNo=2013", f"ApplSeqNum={int(seq)}",
                f"BidApplSeqNum={int(bid_no)}", f"OfferApplSeqNum={int(ask_no)}",
                f"LastPx={0 if is_cancel else int(price)}", f"LastQty={int(qty) * 100}",
                f"ExecType={exectype_asz(int(trade_type))}",
                f"TransactTime={transact_time(dt)}",
            ]
            incs.append((int(seq), dt, "//" + " ".join(kvs)))
    incs.sort(key=lambda x: x[0])
    return incs


def merge_chrono(snaps, incs):
    """快照插入到'其时间戳之前的所有逐笔'之后"""
    out = []
    i = 0
    for snap_dt, snap_line in snaps:
        while i < len(incs) and incs[i][1] <= snap_dt:
            out.append(incs[i][2])
            i += 1
        out.append(snap_line)
    while i < len(incs):
        out.append(incs[i][2])
        i += 1
    return out


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    date = sys.argv[1] if len(sys.argv) > 1 else "20220422"
    instrument = sys.argv[2] if len(sys.argv) > 2 else "000001"
    out_file = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "data", date, f"AX_sbe_szse_{instrument}.log")
    # 第4参数: 快照价格刻度 4=×10^4(默认, py/C++ 引擎与 AX-SBE 历史文件统一) 或 6=×10^6(原生协议)
    snap_price_scale = int(sys.argv[4]) if len(sys.argv) > 4 else 4

    print(f"=== ClickHouse -> AX-SBE 转换 ===")
    print(f"日期: {date}  代码: {instrument}  快照价格刻度: ×10^{snap_price_scale}")
    print(f"输出: {out_file}")

    tmp_dir = os.path.dirname(os.path.abspath(out_file))
    os.makedirs(tmp_dir, exist_ok=True)

    files = fetch_export(date, instrument, tmp_dir)
    snaps = load_snaps(files["tick"], snap_price_scale)
    incs = load_incs(files["order"], files["exe"])
    lines = merge_chrono(snaps, incs)

    with open(out_file, "w", encoding="utf-8", newline="\n") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"  快照 {len(snaps)} 条, 逐笔 {len(incs)} 条, 共 {len(lines)} 条")
    # 顺序自检：逐笔 ApplSeqNum 严格递增、时间戳单调
    prev_seq, prev_dt = None, None
    for line in lines:
        if "ApplSeqNum=" not in line:
            continue
        toks = dict(kv.split("=") for kv in line[2:].split())
        seq = int(toks["ApplSeqNum"])
        dt = datetime.strptime(str(toks["TransactTime"]), "%Y%m%d%H%M%S%f")
        if prev_seq is not None:
            assert seq > prev_seq, f"ApplSeqNum 未严格递增: {seq} <= {prev_seq}"
            assert dt >= prev_dt, f"时间戳随序号回退: {dt} < {prev_dt}"
        prev_seq, prev_dt = seq, dt
    print("  自检通过: ApplSeqNum 严格递增、时间戳单调")


if __name__ == "__main__":
    main()
