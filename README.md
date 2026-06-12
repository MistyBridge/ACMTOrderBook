# ACMTOrderBook

多线程订单簿撮合引擎 — 基于 [AXOrderBook](https://github.com/fpga2u/AXOrderBook) 的 C++ 高性能重写，附带 Python/C++ 对比仪表盘。

---

## 原项目

本项目源自 **[fpga2u/AXOrderBook](https://github.com/fpga2u/AXOrderBook)**，原项目使用 Python 实现 A 股 L2 逐笔行情的订单簿重建、千档快照发布、各档委托队列展示等功能，并计划向 FPGA HLS 迁移。

本项目在此基础上完成了 **完整的 C++ 模块化重写**，并新增了 **GUI 对比仪表盘**，可同时运行 Python 与 C++ 版本并实时对比性能。

---

## 性能对比

测试数据：深交所 000001（平安银行）2022-04-22 全日 L2 逐笔行情，共 **233,875 条消息**。

| 指标 | Python | C++ (-O0) | 提升 |
|------|--------|-----------|------|
| 总耗时 | 61.4 s | 2.4 s | **25.6×** |
| 吞吐量 | 3,112 msg/s | 98,992 msg/s | **31.8×** |
| 重建结果 | ✓ 正确 | ✓ 正确 | 完全一致 |

> 两者产生完全相同的订单簿状态：`NumTrades=81049 LastPx=1606 HighPx=1619 LowPx=1540`

---

## 项目结构

```
ACMTOrderBook/
├── Dashboard.exe          ← 对比仪表盘（双击运行）
├── dashboard.py           ← 仪表盘源码
├── py/
│   ├── main.py            ← Python 入口
│   ├── main.exe           ← Python 编译产物
│   ├── behave/            ← 订单簿引擎核心
│   └── tool/              ← 消息解析工具
├── cpp/
│   ├── main.cpp           ← C++ 入口
│   ├── orderbook.exe      ← C++ 编译产物
│   ├── behave/            ← 订单簿引擎核心
│   └── tool/              ← 消息解析工具
├── data/
│   └── 20220422/          ← 测试数据目录
└── doc/
    └── cpp/plan.md        ← C++ 重写设计文档
```

---

## 仪表盘使用

### 快速启动

双击 `Dashboard.exe` 即可，无需安装 Python 环境。

### 配置

仪表盘顶部有三个可编辑路径：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| Python 入口 | `py/main.exe` | Python 订单簿引擎可执行文件 |
| C++ 入口 | `cpp/orderbook.exe` | C++ 订单簿引擎可执行文件 |
| 数据文件 | `data/20220422/AX_sbe_szse_000001.log` | L2 逐笔行情数据文件 |

可点击 **浏览** 按钮更换文件。

### 运行

1. 点击 **▶ Run Python** 或 **▶ Run C++** 启动对应引擎
2. 进度条实时显示处理进度（约 1000 个检查点）
3. 运行过程中实时更新：速度、已用时间、成交数、最新价、买卖一档
4. 底部日志面板显示完整输出
5. 每次运行 **无视缓存**，始终重新执行；结果同名 **直接覆盖**

### 从源码运行

```bash
# Python
python py/main.py [数据文件路径]

# C++（需先编译）
cd cpp && g++ -std=c++17 -O0 -I. -o orderbook.exe main.cpp behave/*.cpp
./orderbook.exe [数据文件路径]
```

---

## C++ 重写要点

- **模块化设计**：拆分为 `axob_init`、`axob_order`、`axob_trade`、`axob_cage`、`axob_snap` 五个模块
- **数据结构**：`std::map` 价格档有序树 + `std::unordered_map` 订单 O(1) 查表
- **精度处理**：深交所逐笔委托 4 位小数 → 内部 2 位精度计算
- **创业板价格笼子**：300xxx 股票 ±2% 价格笼子完整实现
- **交易阶段**：OpenCall / AMTrading / PMTrading / CloseCall 全阶段覆盖
- **关键修正**：订单簿不从 orderMap 中删除已成交订单（与原 Python 行为一致）

详细设计文档：[doc/cpp/plan.md](doc/cpp/plan.md)

---

## 数据源

测试数据来自深交所 L2 行情，可从以下地址下载后放置于 `data/` 目录下：

链接：[百度盘](https://pan.baidu.com/s/13O7b30DXM64j4WpnNgvXXg)　提取码：`rxif`

- `000001` → `data/20220422/`
- `002594` → `data/20220425/`
- `300750` → `data/20220426/`

---

## 参考

- 原项目：[fpga2u/AXOrderBook](https://github.com/fpga2u/AXOrderBook)
- A 股 L2 行情背景：[交易所L2行情与撮合原理](/doc/SE.md)
- 参考资料：[reference.md](/doc/reference.md)
