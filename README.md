# ACMTOrderBook

**高性能多线程订单簿撮合引擎** — 基于 [AXOrderBook](https://github.com/fpga2u/AXOrderBook) 的 C++ 高性能重写。
2026-08 实测：较 Python 提升 **424 倍**（Windows C++）/**1,002 倍**（Linux C++），
详见[性能对比](#性能对比2026-08-实测)。

---

## 项目简介

ACMTOrderBook 是一个专为 **A 股 L2 逐笔行情** 设计的高性能订单簿重建引擎。本项目将原始 Python 实现重写为 C++，通过多线程架构、数据结构优化、解析优化等技术，实现高性能订单簿重建与 ClickHouse 流式回放。性能数据（2026-08 实测，同源数据 000001 2022-04-22）：Python 5,187 msg/s → Windows C++ 2,200,000 msg/s（424 倍）→ Linux C++ 5,200,000 msg/s（1,002 倍）。

### 核心特性

- 🚀 **极致性能**：5,200,000 msg/s 吞吐量（Linux 实测），0.08μs p50 延迟
- 🧵 **多线程架构**：SPSC 无锁队列，生产者-消费者并行处理
- 📊 **完整功能**：订单簿重建、千档快照发布、委托队列展示
- 🖥️ **GUI 仪表盘**：实时对比 Python 与 C++ 版本性能
- 📈 **版本迭代**：8 个优化版本，持续性能提升

### 技术亮点

**架构优化**：
- SPSC 无锁队列：基于位掩码的环形缓冲区
- 内存池：侵入式空闲链表，减少堆分配
- CPU 亲和性：生产者/消费者绑定不同物理核心

**数据结构优化**：
- HybridLevelBook：紧凑排序数组 + std::map 回退
- 平铺哈希表：ankerl::unordered_dense 提升缓存命中率
- 字段重排：ObOrder 从 40B 压缩到 32B

**解析优化**：
- mmap 文件预加载：消除 File I/O 瓶颈
- 直接字段解析：跳过 map 创建，直接提取字段
- 前向 strstr：记录上次查找位置，减少搜索范围

**延迟优化**：
- genSnap 延迟重建：只在需要时重建快照
- 延迟采样：每 8 条消息采样一次
- 条件拷贝：只拷贝实际类型结构体

---

## ClickHouse 流式回放（cpp_linux，2026-08）

Linux 版引擎（`cpp_linux/`）直连 ClickHouse 的 L2 数据表（`LEVEL2.ORDER_`/`TRANSACTION_`/`TICK_`），
按交易所原生语义重建订单簿，支持深交所（exchange=2）与上交所（exchange=1）。

### 架构（SPSC 单流）

```
ClickHouse ──逐笔单流 (ORDER ∪ TRANSACTION, 服务端全局排序)──► 生产者线程 ──► 无锁环形队列 ──► 引擎消费者
                        ▲ 快照 (TICK 全量驻留, 按时间插入)                                   (批消费 1024)
```

- **数据侧单流**：委托+成交在服务端 `UNION ALL` 并按序列键全局排序（深：`seq_no`；沪：`biz_index` 跨表共享），
  客户端无两路归并；快照按 `t ≤ 快照时间` 前缀插入（对齐参照实现 `mergeChrono` 语义）。
- **SPSC 无锁队列**：生产者线程推入、引擎批消费（Disruptor 理念，一批只经历一次等待）。
- **全量物化**：逐笔单流经 `ch::query` 一次物化驻留（139MB 级），避免流式长连接在大结果集下的
  服务端发送超时断连。
- **精度域**：CH 原始值域（价格 ×10⁴、数量百股 ×10²、金额 ×10⁴）在解析层换算回交易所原生精度，
  引擎内部统一 ×10⁵ 定点。

### 用法

```bash
cd cpp_linux && make
# 凭据经环境变量传入 (不硬编码)
CH_USER=research CH_PASSWORD=your_password ./demo/replay_ch 20260716 600584 1   # 沪市
CH_USER=research CH_PASSWORD=your_password ./demo/replay_ch 20260716 000001 2   # 深市
```

回放结束输出逐帧审计：`NumTrades/Volume/Turnover` 与
`AUDIT total/fullExact/mismatch/avgLvl/call_*/bar_*`（快照按 `num_trades` 键窗口匹配，
连续竞价比 20 档+统计，集合竞价比虚拟撮合盘口，收盘竞价仅比 last）。

### 回归验收

`reg_stream.sh` 全量回归（9 标的 × 5 交易日 + 25 场单日 = 70 场，覆盖沪深两市 5 板块：
SSE_MB/STAR/SZSE_MB/SME/GEM）与参照实现输出**逐位一致**（含审计列与 Volume/Turnover 域）。

---

## 性能对比（2026-08 实测）

**测试基准与数据**：深交所 000001（平安银行）2022-04-22 全日 L2 逐笔行情，共 **233,615 条消息**
（逐笔委托 122,359 + 逐笔成交 106,434 + 十档快照 4,822；深交所 L2 快照深度为 10 档，
千档委托队列 ORDERQUEUE 不在测试样例中，引擎亦不消费）。全部为单次重放实测。

| 实现 | 运行环境 | 吞吐量 | p50 | p99 | p99.9 | pmax |
|------|----------|--------|-----|-----|-------|------|
| Python (`py/`) | 本地 Windows，Python 3.11.9 | 5,187 msg/s | - | - | - | - |
| C++ v2 (`cpp v2`) | 本地 Windows，MinGW g++ 13.2 (-O3 -march=native) | 2,200,000 msg/s | 0.2 μs | 395 μs | 960 μs | 1,050 μs |
| C++ Linux (`cpp_linux`) | Linux 服务器，g++ -O3 -march=native，ClickHouse 直连 | 5,200,000 msg/s | 0.08 μs | 0.36 μs | 1.49 μs | 1,680 μs |

**各实现的测试方式**：
- **Python**：`python py/main.py data/20220422/AX_sbe_szse_000001.log 1`（本地 Windows 实测 45.0s）
- **C++ v2 (Windows)**：`orderbook_v2.exe <log> 0 2 16384 64 1`（SPSC 生产者 Core0/消费者 Core2，
  队列 16384、批次 64；实测 0.104-0.109s；延迟为端到端采样统计）。原 README 中 1,339,869 msg/s
  等数字为**原项目在 py 仪表盘（dashboard.py，Windows 本地驱动）环境的历史基准**，当前环境
  以本表实测为准。
- **C++ Linux**：`replay_ch 20220422 000001 2 127.0.0.1 8123 bench`（ClickHouse 直连，数据与
  Windows 版本地文件完全同源；延迟为**事件级 onMsg 处理耗时**（全量计时），与 Windows 版端到端
  采样口径不同；吞吐为含延迟统计口径，去掉统计开销 ~7.9M msg/s）

> 三个实现重建的订单簿状态一致：`NumTrades=810,490 LastPx=1606 HighPx=1619 LowPx=1540`

---

## 版本说明

### CPP v1 — 单线程版本
基础 C++ 重写，包含完整的订单簿功能实现。

### CPP v2 — 多线程优化版本 🚀
基于 v1 进行多线程优化，包含以下核心优化：

**架构优化**：
- **SPSC 无锁队列**：生产者-消费者线程间数据传输
- **内存池**：减少动态内存分配开销
- **缓存行填充**：防止 false sharing
- **批量处理**：减少原子操作开销
- **CPU 亲和性绑定**：生产者/消费者绑定不同物理核心
- **延迟测量**：p50/p99/p99.9/pmax 全链路延迟统计

**数据结构优化**：
- **HybridLevelBook**：紧凑排序数组（n≤256）+ std::map 回退（n>256）
- **平铺哈希表**：ankerl::unordered_dense 提升缓存命中率
- **字段重排**：ObOrder 从 40B 压缩到 32B

**解析优化**：
- **mmap 文件预加载**：消除 File I/O 瓶颈
- **直接字段解析**：跳过 map 创建，直接提取字段
- **前向 strstr**：记录上次查找位置，减少搜索范围 ~4x
- **零分配解析**：消除堆分配开销

**延迟优化**：
- **genSnap 延迟重建**：只在需要时重建快照
- **延迟采样**：每 8 条消息采样一次，减少 QPC 开销
- **条件拷贝**：只拷贝实际类型结构体

**编译优化**：
- **MSVC 2022**：/O2 /GL /arch:AVX2 /LTCG
- **PGO**：Profile-Guided Optimization
- **Huge Pages**：大页内存优化

---

## 编译指南

### 环境要求

- **操作系统**：Windows 10/11
- **编译器**：MSVC 2022 (Visual Studio 2022)
- **CMake**：3.15 或更高版本
- **Git**：用于克隆项目

### 编译步骤

#### 1. 克隆项目

```bash
git clone https://github.com/MistyBridge/ACMTOrderBook.git
cd ACMTOrderBook
```

#### 2. 编译 Python 版本（可选）

```bash
cd py
python main.py
```

#### 3. 编译 C++ v1 版本

```bash
cd "cpp v1"
g++ -std=c++17 -O0 -I. -o orderbook.exe main.cpp behave/*.cpp
```

#### 4. 编译 C++ v2 版本（推荐）

```bash
cd "cpp v2"

# 使用 CMake 配置（推荐）
cmake -B build -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=ON -DUSE_FLAT_HASHMAP=1

# 编译 Release 版本
cmake --build build --config Release --parallel 8

# 输出文件
build/Release/orderbook_v2.exe
```

#### 5. 编译优化选项

**基础优化**（已包含）：
- `/O2`：最大速度优化
- `/GL`：全程序优化
- `/arch:AVX2`：AVX2 指令集
- `/LTCG`：链接时代码生成

**高级优化**（可选）：
```bash
# PGO 优化（需要运行两次）
cmake -B build_pgo -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=ON -DUSE_FLAT_HASHMAP=1 -DPGO_MODE=GEN
cmake --build build_pgo --config Release --parallel 8
./build_pgo/Release/orderbook_v2.exe [数据文件] 0 2 16384 64 100

cmake -B build_pgo_opt -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=ON -DUSE_FLAT_HASHMAP=1 -DPGO_MODE=USE
cmake --build build_pgo_opt --config Release --parallel 8
```

#### 6. 运行测试

```bash
# 单次测试
./build/Release/orderbook_v2.exe [数据文件] [生产者核心] [消费者核心] [队列容量] [批次大小] [重放次数]

# 示例：100 次重放测试
./build/Release/orderbook_v2.exe "../data/20220422/AX_sbe_szse_000001.log" 0 2 16384 64 100
```

### 性能优化建议

1. **使用 Release 模式**：确保编译时使用 Release 配置
2. **启用 AVX2**：确保 CPU 支持 AVX2 指令集
3. **绑定 CPU 核心**：生产者绑定 Core 0，消费者绑定 Core 2
4. **使用 mmap**：启用 mmap 文件预加载（默认开启）
5. **使用平铺哈希表**：启用 ankerl::unordered_dense（默认开启）

---

## 版本演进

> 下表中 v1~v2.8 为**原项目历史基准**（py 仪表盘 Windows 本地环境测得，当前环境未复测，
> 供演进脉络参考）；**实测行以"性能对比"章节为准**。

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
| C++ Linux（实测） | 5,200,000 msg/s | ClickHouse 直连回放 + SPSC 单流 + 全量物化 |

---

## 项目结构

```
ACMTOrderBook/
├── dashboard.py           ← 对比仪表盘源码
├── py/
│   ├── main.py            ← Python 入口
│   ├── behave/            ← 订单簿引擎核心
│   └── tool/              ← 消息解析工具
├── cpp v1/                ← 单线程版本
│   ├── main.cpp           ← C++ v1 入口
│   ├── behave/            ← 订单簿引擎核心
│   └── tool/              ← 消息解析工具
├── cpp v2/                ← 多线程优化版本（Windows）
│   ├── main.cpp           ← C++ v2 入口
│   ├── core/              ← 基础组件 (SPSC队列、内存池、缓存行、CPU亲和性、延迟统计)
│   ├── pipeline/          ← 管道架构 (生产者/消费者)
│   ├── behave/            ← 订单簿引擎核心
│   ├── tool/              ← 消息解析工具
│   └── source/            ← ClickHouse 数据源 (ch_client/clickhouse_source, 双平台)
├── cpp_linux/             ← Linux 版引擎 + ClickHouse 流式回放
│   ├── src/api.cpp        ← C API (快照校验闭环/1s 聚合校验)
│   ├── demo/replay_ch.cpp ← 回放入口 (凭据走 CH_USER/CH_PASSWORD 环境变量)
│   └── reg_stream.sh      ← 70 场全量回归脚本
└── data/                  ← 测试数据 (体积大, 不入仓库, 见"数据源")
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
| C++ 入口 | `cpp v2/orderbook_v2.8.exe` | C++ v2.8 多线程优化版本（最终版） |
| 数据文件 | `cpp v2/test_data.log` | L2 逐笔行情数据文件 |
| 重放次数 | `1` | 数据重放次数（1-1000），用于压力测试 |

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

# C++ v1（需先编译）
cd "cpp v1" && g++ -std=c++17 -O0 -I. -o orderbook.exe main.cpp behave/*.cpp
./orderbook.exe [数据文件路径]

# C++ v2（需先编译）
cd "cpp v2"
cmake -B build -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=ON -DUSE_FLAT_HASHMAP=1
cmake --build build --config Release --parallel 8
./build/Release/orderbook_v2.exe [数据文件路径] [生产者核心] [消费者核心] [队列容量] [批次大小] [重放次数]
```

---

## C++ 重写要点

### 基础架构
- **模块化设计**：拆分为 `axob_init`、`axob_order`、`axob_trade`、`axob_cage`、`axob_snap` 五个模块
- **数据结构**：`std::map` 价格档有序树 + `std::unordered_map` 订单 O(1) 查表
- **精度处理**：深交所逐笔委托 4 位小数 → 内部 2 位精度计算
- **创业板价格笼子**：300xxx 股票 ±2% 价格笼子完整实现
- **交易阶段**：OpenCall / AMTrading / PMTrading / CloseCall 全阶段覆盖
- **关键修正**：订单簿不从 orderMap 中删除已成交订单（与原 Python 行为一致）

### CPP v2 多线程优化
- **SPSC 无锁队列**：基于位掩码的环形缓冲区，避免模运算开销
- **内存池**：侵入式空闲链表，2x 增长策略，减少内存碎片
- **缓存行对齐**：`alignas(64)` 防止 false sharing
- **批量处理**：每批最多 64 条消息，减少原子操作次数
- **CPU 亲和性**：生产者绑定 Core 0，消费者绑定 Core 2
- **延迟统计**：环形缓冲区 + nth_element 实现 O(n) 分位数计算
- **编译器优化**：MSVC 2022 /O2 /GL /arch:AVX2 /LTCG
- **字段重排**：ObOrder 从 40B 压缩到 32B，ObExec 优化到 40B

### 数据结构优化
- **HybridLevelBook**：紧凑排序数组（n≤256）+ std::map 回退（n>256），价格档操作提速 3.5x
- **平铺哈希表**：ankerl::unordered_dense 提升缓存命中率
- **内存预取**：`__builtin_prefetch` 预取下一条消息到 L1 缓存

### 解析优化
- **mmap 文件预加载**：消除 File I/O 瓶颈，123MB/395ms 读取速度
- **直接字段解析**：跳过 map 创建，直接提取字段，节省 ~370ns/消息
- **前向 strstr**：记录上次查找位置，减少搜索范围 ~4x
- **零分配解析**：消除堆分配开销，使用栈缓冲区

### 延迟优化
- **genSnap 延迟重建**：只在需要时重建快照，减少 60-80% 的 genSnap 调用
- **延迟采样**：每 8 条消息采样一次，减少 QPC 开销
- **条件拷贝**：只拷贝实际类型结构体，减少 memcpy 开销

### 编译优化
- **MSVC 2022**：/O2 /GL /arch:AVX2 /LTCG
- **PGO**：Profile-Guided Optimization，+0.8% 吞吐量提升
- **Huge Pages**：大页内存优化，+0.5% 吞吐量提升

---

## 数据源

测试数据来自深交所 L2 行情，可从以下地址下载后放置于 `data/` 目录下（体积大，不入仓库）：

链接：[百度盘](https://pan.baidu.com/s/13O7b30DXM64j4WpnNgvXXg)　提取码：`rxif`

- `000001` → `data/20220422/`
- `002594` → `data/20220425/`
- `300750` → `data/20220426/`

---

## 参考

- 原项目：[fpga2u/AXOrderBook](https://github.com/fpga2u/AXOrderBook)

---

### 版本吞吐量数据（历史基准）

> 以下为**原项目历史基准**（py 仪表盘 Windows 本地环境，2022 版数据与测试方式），
> 供版本演进脉络参考；当前实测见"性能对比"章节。

| 版本 | 吞吐量 (msg/s) | 相对提升 |
|------|----------------|----------|
| Python | 4,109 | 基准 |
| C++ v1 | 64,613 | +1,472% |
| C++ v2.1 | 223,410 | +5,337% |
| C++ v2.2 | 433,332 | +10,446% |
| C++ v2.3 | 1,102,511 | +26,730% |
| C++ v2.4 | 1,087,261 | +26,361% |
| C++ v2.5 | 1,065,053 | +25,819% |
| C++ v2.6 | 1,240,251 | +30,083% |
| C++ v2.7 | 1,224,407 | +29,696% |
| C++ v2.8 | 1,339,869 | +32,509% |

### 性能提升总结（2026-08 实测口径）

基于同源测试数据（000001 2022-04-22，233,615 条）的当前实测：

- **Python → C++ v2 (Windows)**: 5,187 → 2,200,000 msg/s = **+42,400%（424 倍）**
- **Python → C++ Linux**: 5,187 → 5,200,000 msg/s = **+100,200%（1,002 倍）**
- **C++ v2 (Windows) → C++ Linux**: 2,200,000 → 5,200,000 msg/s = **+136%**
