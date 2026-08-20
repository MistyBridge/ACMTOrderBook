# cpp_linux — Linux/GCC 订单簿引擎共享库

A股 L2 订单簿重建引擎的 Linux 共享库版本 (libacmt_orderbook.so),引擎源码直接引用
`../cpp v2` 下的 `behave/` 与 `source/`(单一源码树,不复制)。
**数据源为 ClickHouse 直连,无本地文件输入。**

## 构建

```bash
cd cpp_linux
ln -sfn "../cpp v2" engine      # 首次: 引擎源码符号链接 (规避路径空格)
make all                        # 产出 libacmt_orderbook.so + demo/replay_ch
```

要求: g++ ≥ 9 (C++17), x86-64。HTTP 客户端为自实现 (HTTP/1.0 + POSIX 套接字,
Windows 侧为 WinHTTP,同一源码)。

## 接口 (include/acmt_orderbook.h)

C API (`extern "C"`, 配合 `-fvisibility=hidden` 仅导出符号):

| 函数 | 说明 |
|--|--|
| `acmt_ob_create/destroy` | 创建/销毁实例 (instrument, exchange=2 深交所) |
| `acmt_ob_on_order/on_exec/on_snap` | 逐条事件输入 (实时网关可走此路径) |
| `acmt_ob_replay_ch` | **直连 ClickHouse 拉取一个交易日并回放** |
| `acmt_ob_get_book/get_stat` | 重建簿 10 档 / 累计统计 |

单位约定(C API,深交所): 价格 ×10⁴、数量 ×10²、成交额 ×10⁴ 元、时间
YYYYMMDDHHMMSSsss(北京时间);上交所为 价格 ×10³、数量 ×10³、成交额 ×10⁵。
即**接口沿用交易所快照原始精度**。

引擎内部则统一定点 **×10⁵**(价格/数量/金额同标度,int64),换算在 `.so` 内完成:

- 最小可表示单位 1e-5,覆盖深交所 2 位/上交所 3 位小数,并为碎股(境外市场可至
  1e-5 股)预留空间;基金、可转债的 3 位小数价格不再需要特例分支。
- 价格必须 int64:×10⁵ 下 int32 上限仅 21474.83 元。
- 成交额累加用 `__int128` 中间量:涨停封单单笔量 × 高价股价格可达 1.3e21,
  超 int64 上限 143 倍。

## 使用示例

```bash
# 凭据经环境变量传入: CH_USER / CH_PASSWORD
CH_USER=research CH_PASSWORD=your_password ./demo/replay_ch 20260713 000001   # 默认 127.0.0.1:8123
CH_USER=research CH_PASSWORD=your_password ./demo/replay_ch 20220422 000001 10.1.2.3 9000
```

## 验证

2026-07-13 与 2022-04-22 两天 SQL 直连回放,与 Windows 版及 ClickHouse 快照基准
逐项一致(成交笔数/量/额、开高低收、重建簿 5 档)。

## 待办

- 上交所 (exchange=1) 支持: ClickHouseSource 的 SSE 路径 + 快照涨跌停规则表
- 实时 API 网关接入 (事件流式喂入 acmt_ob_on_* 即可, 接口已就绪)
- 多标的并发与性能调优
