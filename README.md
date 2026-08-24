# ACMTOrderBook

**高性能多线程订单簿撮合引擎** — 基于 [AXOrderBook](https://github.com/fpga2u/AXOrderBook) 的 C++ 高性能重写，专为 **A 股 L2 逐笔行情**的订单簿重建设计。

> 采用 **SPSC 无锁队列 + 读写分离双线程**架构，结合零拷贝 mmap、HybridLevelBook、平铺哈希、大页缓存等优化。
> 在真实 L2 数据（深交所 000001 2022-04-22，233,875 条）上实测：Python `1,529 msg/s` → C++ v2 `≈1,330,000 msg/s`（`T1 系统端到端吞吐`）。

---

## 核心特性

- 🚀 **极致性能**：真实数据实测 C++ v2 系统端到端 ≈`1,330,000 msg/s`（详见[性能测试](#性能测试)）
- 🧵 **双线程读写分离**：SPSC 无锁队列，生产者/消费者并行处理
- 📊 **完整能力**：逐笔订单簿重建、十档快照、委托队列展示
- 🖥️ **GUI 仪表盘**：`dashboard.py` 对比 Python 与 C++ 版本
- 🔀 **跨实现**：Python 参照实现 + C++ v1 / v2（Windows）+ Linux 共享库（`cpp_linux`，ClickHouse 直连）

---

## 性能测试

**测试基准与数据**：深交所 000001（平安银行）2022-04-22 全日 L2，共 **233,875 条消息**
（逐笔委托 122,359 + 逐笔成交 106,434 + 十档快照 5,082；深交所 L2 快照深度 10 档，千档委托队列不在样例中）。单次重放实测。

**测试环境**：Windows 沙箱，MinGW g++ 8.1.0（`-O3 -march=native`），QPC 分辨率 ≈100 ns。

**度量口径**：

- **吞吐 = T1 系统端到端吞吐**：总消息数 ÷ 整段墙钟时间（含 mmap 读、解析、生产者、SPSC 队列、双线程并行、引擎），**反映全部架构 + 数据路径 + 引擎优化**——双线程、零拷贝/mmap、批量/预取、大页缓存等都体现在 T1。
- **延迟 = L1 单事件处理延迟**：每条真实消息 `onMsg()` 的处理耗时，**逐条全量计时**，报告 `p50 / p99 / p99.9 / pmax`。
- **计时约定**：单调时钟（Windows `QueryPerformanceCounter`，Linux `steady_clock`，Python `time.perf_counter_ns()`）；百分位索引 `floor(p×(N-1))`。

| 实现 | 运行环境 | **T1 系统端到端吞吐** | p50 | p99 | p99.9 | pmax | 正确性 |
|------|----------|------------------|-----|-----|-------|------|--------|
| Python (`py/`) | 本地 Windows，Python 3.10 | 1,529 msg/s | 413 μs | 2,411 μs | 4,152 μs | 957 ms | ✅ |
| C++ v1 (`cpp v1`) | 本地 Windows，MinGW g++ 8.1.0 (-O3 -march=native) | 159,434 msg/s | 0.3 μs | 1.1 μs | 8.9 μs | 3.0 ms | ✅ |
| C++ v2 (`cpp v2`) | 本地 Windows，MinGW g++ 8.1.0 (-O3 -march=native) | ≈1,330,000 msg/s | 0.1 μs | 0.5 μs | 0.9 μs | ≈1.0 ms | ✅ |

---

## 版本说明

### CPP v1 — 单线程版本
基础 C++ 重写，含完整订单簿功能（默认 `-O0` 编译）。

### CPP v2 — 多线程优化版本 🚀

**架构优化**
- SPSC 无锁队列：生产者-消费者线程间数据传输，位掩码环形缓冲区
- 内存池：侵入式空闲链表，减少动态分配
- 缓存行填充：`alignas(64)` 防 false sharing
- 批量处理：每批最多 64 条，减少原子操作
- CPU 亲和性：生产者/消费者绑定不同物理核心
- 延迟测量：逐条全量，`onMsg()` 单事件计时

**数据结构优化**
- HybridLevelBook：紧凑排序数组（n≤256）+ std::map 回退（n>256）
- 平铺哈希表：ankerl::unordered_dense 提升缓存命中率
- 字段重排：ObOrder 从 40B 压缩到 32B，ObExec 优化到 40B

**解析优化**
- mmap 文件预加载：消除 File I/O 瓶颈
- 直接字段解析：跳过 map 创建，直接提取字段
- 前向 strstr：记录上次查找位置，减少搜索范围 ~4x
- 零分配解析：消除堆分配开销

**延迟 / 编译优化**
- genSnap 延迟重建：只在需要时重建快照
- 条件拷贝：只拷贝实际类型结构体
- 编译：MSVC 2022 `/O2 /arch:AVX2 /LTCG`；可选 PGO、Huge Pages

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

## 版本演进（历史基准）

> v1~v2.8 为**原项目历史基准**（py 仪表盘 Windows 本地环境测得，未复测，仅供演进脉络参考）；当前实测见[性能测试](#性能测试)。

| 版本 | 吞吐量（历史基准） | 核心优化 |
|------|--------|----------|
| Python | 4,109 msg/s | 基准 |
| CPP v1 | 64,613 msg/s | 单线程 C++ 重写 |
| CPP v2.1 | 223,410 msg/s | MSVC 编译优化 + HybridLevelBook |
| CPP v2.2 | 433,332 msg/s | mmap 文件预加载 + 平铺哈希表 |
| CPP v2.3 | 1,102,511 msg/s | 直接字段解析优化 |
| CPP v2.4 | 1,087,261 msg/s | 代码质量优化 |
| CPP v2.5 | 1,065,053 msg/s | PGO + Huge Pages |
| CPP v2.6 | 1,240,251 msg/s | 零分配解析 + 二分查找 + ankerl |
| CPP v2.7 | 1,224,407 msg/s | genSnap 延迟重建 |
| CPP v2.8 | 1,339,869 msg/s | 前向 strstr + 延迟采样 + 条件拷贝 |

---

## 项目结构

```
ACMTOrderBook/
├── dashboard.py           ← 对比仪表盘源码
├── py/                    ← Python 参照实现
│   ├── main.py
│   ├── behave/            ← 订单簿引擎核心
│   └── tool/              ← 消息解析工具
├── cpp v1/                ← 单线程 C++ 版本
│   ├── main.cpp
│   ├── behave/            ← 订单簿引擎核心
│   └── tool/
├── cpp v2/                ← 多线程优化版本（Windows）
│   ├── main.cpp
│   ├── core/              ← 基础组件 (SPSC 队列、内存池、缓存行、CPU 亲和性、延迟统计)
│   ├── pipeline/          ← 管道架构 (生产者/消费者)
│   ├── behave/            ← 订单簿引擎核心
│   ├── tool/              ← 消息解析工具
│   └── source/            ← ClickHouse 数据源 (ch_client/clickhouse_source, 双平台)
├── cpp_linux/             ← Linux 版引擎 + ClickHouse 流式回放
│   ├── src/api.cpp        ← C API (快照校验闭环 / 1s 聚合校验)
│   ├── demo/replay_ch.cpp ← 回放入口 (CH_USER/CH_PASSWORD)
│   └── plot_throughput.py ← 吞吐量曲线脚本
└── data/                  ← 测试数据 (体积大, 不入仓库, 见"数据源")
```

---

## 仪表盘使用

双击 `Dashboard.exe` 即可（无需 Python 环境）。顶部可编辑 Python/C++ 入口与数据文件路径；运行中实时显示速度、已用时间、成交数、最新价、买卖一档；底部日志面板显示完整输出。每次运行**无视缓存**、结果直接覆盖。

**从源码运行**
```bash
python py/main.py [数据文件路径]
cd "cpp v1" && g++ -std=c++17 -O0 -I. -o orderbook.exe main.cpp behave/*.cpp && ./orderbook.exe [数据文件路径]
cd "cpp v2" && cmake -B build -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=ON -DUSE_FLAT_HASHMAP=1
cmake --build build --config Release --parallel 8
./build/Release/orderbook_v2.exe [数据文件路径] [生产者核心] [消费者核心] [队列容量] [批次大小] [重放次数]
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

---

## 全历史吞吐量性能曲线

![全历史吞吐量性能曲线](throughput.png)

- 蓝色曲线：**历史基准**（原项目各版本，py 仪表盘 Windows 环境测得，未复测）
- 红色曲线：**本机实测 T1 系统端到端**（000001 2022-04-22，233,875 条，纵轴对数）
- 曲线脚本：`cpp_linux/plot_throughput.py`
