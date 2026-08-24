# ACMTOrderBook

**高性能多线程订单簿撮合引擎** — 基于 [AXOrderBook](https://github.com/fpga2u/AXOrderBook) 的 C++ 高性能重写，专为 **A 股 L2 逐笔行情**的订单簿重建设计。

> 采用 **SPSC 无锁队列 + 读写分离双线程**架构，结合零拷贝 mmap、HybridLevelBook、平铺哈希、大页缓存等优化。
> 版本演进在真实 L2 数据（深交所 000001 2022-04-22，233,875 条）上实测：Python `1,575 msg/s` → C++ v2.8 `≈1.27M msg/s`（`T1 系统端到端吞吐`，约 **800×** 提升）。

---

## 核心特性

- 🚀 **极致性能**：全版本演进在真实数据上实测，C++ v2.8 系统端到端 ≈`1.27M msg/s`（详见[性能测试](#性能测试)）
- 🧵 **双线程读写分离**：SPSC 无锁队列，生产者/消费者并行处理
- 📊 **完整能力**：逐笔订单簿重建、十档快照、委托队列展示
- 🖥️ **GUI 仪表盘**：`dashboard.py` 对比 Python 与 C++ 版本
- 🔀 **跨实现**：Python 参照实现 + C++ v1 / v2（Windows）+ Linux 共享库（`cpp_linux`，ClickHouse 直连）
- 🗂 **版本管理**：`versions/` 收录各历史版本源码（按 commit 检出，见[版本管理](#版本管理)）

---

## 性能测试

**测试基准与数据**：深交所 000001（平安银行）2022-04-22 全日 L2，共 **233,875 条消息**
（逐笔委托 122,359 + 逐笔成交 106,434 + 十档快照 5,082；深交所 L2 快照深度 10 档，千档委托队列不在样例中）。单次重放实测。

**测试环境**：Windows 沙箱，MinGW g++ 8.1.0（`-O3 -march=native -funroll-loops -flto`），QPC 分辨率 ≈100 ns。

**度量口径**：

- **吞吐 = T1 系统端到端吞吐**：总消息数 ÷ 整段墙钟时间（含文件读/mmap、解析、生产者、SPSC 队列、双线程并行、引擎），**反映全部架构 + 数据路径 + 引擎优化**——双线程、零拷贝/mmap、批量/预取、大页缓存等都体现在 T1。
- **延迟 = L1 单事件处理延迟**：每条真实消息 `onMsg()` 的处理耗时，**逐条全量计时**，报告 `p50 / p99 / p99.9 / pmax`。
- **计时约定**：单调时钟（Windows `QueryPerformanceCounter`，Linux `steady_clock`，Python `time.perf_counter_ns()`）；百分位索引 `floor(p×(N-1))`。

### 版本演进实测（T1 系统端到端，本环境实测）

| 版本 | **T1 吞吐量** | 核心优化 | 正确性 |
|------|------------|----------|--------|
| Python (`py/`) | 1,575 msg/s | 基线参照 | ✅ |
| C++ v1 | 101,405 msg/s | 单线程 C++ 重写 | ✅ |
| C++ v2 | 103,559 msg/s | SPSC 无锁双线程 | ✅ |
| C++ v2.1 | 103,908 msg/s | HybridLevelBook + 预取 | ✅ |
| C++ v2.2 | 256,480 msg/s | mmap 文件预加载 + 平铺哈希 | ✅ |
| C++ v2.3 | 902,177 msg/s | 直接字段解析 | ✅ |
| C++ v2.4 | 936,373 msg/s | 代码质量优化 | ✅ |
| C++ v2.7 | 1,112,911 msg/s | genSnap 延迟重建 | ✅ |
| C++ v2.8 | 1,272,775 msg/s | 前向 strstr + 延迟采样 + 条件拷贝 | ✅ |

> ✅ 各版本重建的订单簿状态一致：`NumTrades=81,049  LastPx=1606  HighPx=1619  LowPx=1540  OpenPx=1564  TVol=9,212,740,800  TVal=14,684,529,579,500`。
> ⚠️ v2.5 / v2.6 为演进过程中描述过、但在本仓库线性历史上无独立可检出 commit 的中间态，故未列入实测表。
> 📌 历史版本内置的延迟统计使用 `high_resolution_clock`，在本沙箱量化到 ~1ms，p50/p99 不可信；故上表仅列 T1 吞吐量。

### 当前版本延迟（C++ v2.8，L1 单事件 onMsg，QPC）

在真实数据上逐条实测：`p50=0.1µs  p99=0.6µs  p99.9=1.1µs  pmax≈1037µs`（T1 ≈`1,352,950 msg/s`）。

---

## 全历史吞吐量性能曲线

![全历史吞吐量性能曲线](throughput.png)

- 红色曲线：**本机实测 T1 系统端到端**（000001 2022-04-22，233,875 条，纵轴对数）
- 曲线脚本：`cpp_linux/plot_throughput.py`

---

## 版本说明

- **`py/`** — Python 参照实现（行为基准）
- **`cpp v1/`** — 单线程 C++ 版本（基础重写，含完整订单簿功能）
- **`cpp v2/`** — 多线程优化版本，即当前引擎（含 SPSC 队列、mmap、HybridLevelBook、L1 延迟统计等）
- **`cpp_linux/`** — Linux 版引擎 + ClickHouse 流式回放（`src/api.cpp`、`demo/replay_ch.cpp`）
- **`versions/`** — 历史版本源码归档（见下方版本管理）

---

## 版本管理

`versions/` 按 commit 检出各历史版本的源码，保留完整演进脉络，便于对比与复测。

| 版本目录 | 检出 commit | 说明 |
|----------|------------|------|
| `versions/v1` | `15cd00d` | 单线程 C++ 基线 |
| `versions/v2` | `3a03928` | SPSC 双线程 |
| `versions/v2.1` | `40bcac2` | HybridLevelBook + 预取 |
| `versions/v2.2` | `a4fbf49` | mmap 文件预加载 |
| `versions/v2.3` | `ae984dd` | 直接字段解析 |
| `versions/v2.4` | `77c40eb` | 代码质量优化 |
| `versions/v2.7` | `0f2c9dd` | genSnap 延迟重建 |
| `versions/v2.8` | `15cd00d` | 前向 strstr + 延迟采样 |

**MinGW 兼容性说明**：这些历史版本为 MSVC 环境开发，在 MinGW 下编译需少量兼容修补（**不改动引擎逻辑**）：

- `tool/field_parser.h`：为 GCC/MinGW 引入 `<cpuid.h>`，并将 `__cpuid` 替换为便携的 `__get_cpuid`（SSE4.2 检测）。
- `core/huge_pages.h`：原文件为平台相关、未随源码提交；以 `versions/v2.7`、`versions/v2.8` 下的 `core/huge_pages.h` 桩文件提供 `allocLargePages`/`freeLargePages` 回退（返回 `nullptr`，`MemoryPool` 自动退回 `alignedAlloc`，即"大页不可用"场景）。
- `tool/axsbe_base.h`（v2.4）：补充 `<cstring>`/`<cstdlib>`，供模板解析使用 `strstr`/`strncmp`/`strtoll`。

---

## 编译指南

### 环境要求
- Windows 10/11；MSVC 2022（Visual Studio 2022）；CMake ≥ 3.15；Git

### 编译步骤

**1. 克隆项目**
```bash
git clone https://github.com/MistyBridge/ACMTOrderBook.git
cd ACMTOrderBook
```

**2. Python 版本（可选）**
```bash
cd py && python main.py
```

**3. C++ v1**
```bash
cd "cpp v1"
g++ -std=c++17 -O0 -I. -o orderbook.exe main.cpp behave/*.cpp
```

**4. C++ v2（推荐，CMake + MSVC）**
```bash
cd "cpp v2"
cmake -B build -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=ON -DUSE_FLAT_HASHMAP=1
cmake --build build --config Release --parallel 8
# 输出: build/Release/orderbook_v2.exe
```

**5. 运行测试**
```bash
./build/Release/orderbook_v2.exe [数据文件] [生产者核心] [消费者核心] [队列容量] [批次大小] [重放次数]
# 示例
./build/Release/orderbook_v2.exe "../data/AX_sbe_szse_000001/AX_sbe_szse_000001.log" 0 2 16384 64 1
```

### 性能优化建议
1. 使用 **Release** 模式编译
2. 确保 CPU 支持 **AVX2**
3. 生产者绑 Core 0、消费者绑 Core 2
4. 启用 **mmap**（默认开启）与**平铺哈希表**（ankerl，默认开启）

---

## 项目结构

```
ACMTOrderBook/
├── dashboard.py             ← 对比仪表盘源码
├── py/                      ← Python 参照实现
│   ├── main.py
│   ├── behave/              ← 订单簿引擎核心
│   └── tool/                ← 消息解析工具
├── cpp v1/                  ← 单线程 C++ 版本
├── cpp v2/                  ← 多线程优化版本（Windows, 当前引擎）
│   ├── main.cpp
│   ├── core/                ← 基础组件 (SPSC 队列、内存池、缓存行、CPU 亲和性、延迟统计)
│   ├── pipeline/            ← 管道架构 (生产者/消费者)
│   ├── behave/              ← 订单簿引擎核心
│   ├── tool/                ← 消息解析工具 (mmap/field_parser)
│   └── source/              ← ClickHouse 数据源 (ch_client/clickhouse_source)
├── cpp_linux/               ← Linux 版引擎 + ClickHouse 流式回放
│   ├── src/api.cpp          ← C API (快照校验闭环 / 1s 聚合校验)
│   ├── demo/replay_ch.cpp   ← 回放入口 (CH_USER/CH_PASSWORD)
│   └── plot_throughput.py   ← 吞吐量曲线脚本
├── versions/                ← 历史版本源码归档 (v1 ~ v2.8)
├── throughput.png           ← 版本演进实测曲线
└── data/                    ← 测试数据 (体积大, 不入仓库, 见"数据源")
```

---

## C++ 重写要点

- **模块化**：`axob_init` / `axob_order` / `axob_trade` / `axob_cage` / `axob_snap`
- **数据结构**：价格档有序树 + 订单 O(1) 查表
- **精度处理**：深交所逐笔委托 → 统一定点 ×10⁵（价格/数量/金额同标度，int64）
- **创业板价格笼子**：300xxx 股票 ±2% 有效竞价范围
- **交易阶段**：OpenCall / AMTrading / PMTrading / CloseCall 全覆盖
- **关键修正**：订单簿不从 orderMap 删除已成交订单（与原 Python 行为一致）

---

## 数据源

真实 L2 测试数据来自深交所行情，体积大、**不入仓库**（`.gitignore` 已排除 `data/`）。示例：
```
data/AX_sbe_szse_000001/AX_sbe_szse_000001.log
data/AX_sbe_szse_300750/AX_sbe_szse_300750.log
```
数据获取方式：历史 L2 样例见[百度盘](https://pan.baidu.com/s/13O7b30DXM64j4WpnNgvXXg)（提取码 `rxif`），或经 `cpp_linux` 直连公司内网 ClickHouse（`LEVEL2.ORDER_` / `TRANSACTION_` / `TICK_` 表，凭据走 `CH_USER`/`CH_PASSWORD`）。

---

## 参考

- 原项目：[fpga2u/AXOrderBook](https://github.com/fpga2u/AXOrderBook)
