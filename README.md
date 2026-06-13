# ACMTOrderBook

多线程订单簿撮合引擎 — 基于 [AXOrderBook](https://github.com/fpga2u/AXOrderBook) 的 C++ 高性能重写，附带 Python/C++ 对比仪表盘。

---

## 原项目

本项目源自 **[fpga2u/AXOrderBook](https://github.com/fpga2u/AXOrderBook)**，原项目使用 Python 实现 A 股 L2 逐笔行情的订单簿重建、千档快照发布、各档委托队列展示等功能，并计划向 FPGA HLS 迁移。

本项目在此基础上完成了 **完整的 C++ 模块化重写**，并新增了 **GUI 对比仪表盘**，可同时运行 Python 与 C++ 版本并实时对比性能。

---

## 版本说明

### CPP v1 — 单线程版本
基础 C++ 重写，包含完整的订单簿功能实现。

### CPP v2 — 多线程优化版本 🚀
基于 v1 进行多线程优化，包含 7 项核心优化：
- **SPSC 无锁队列**：生产者-消费者线程间数据传输
- **内存池**：减少动态内存分配开销
- **缓存行填充**：防止 false sharing
- **批量处理**：减少原子操作开销
- **CPU 亲和性绑定**：生产者/消费者绑定不同物理核心
- **延迟测量**：p50/p99/p99.9/pmax 全链路延迟统计
- **编译器优化**：-O3 -march=native -flto

### CPP v2.1 — 性能优化版本 ⚡
在 v2 基础上进一步优化，性能提升 **+84%**：
- **HybridLevelBook**：紧凑排序数组（n≤128）+ std::map 回退（n>128）
- **显式模板实例化**：减少编译时间
- **关键函数强制内联**：减少函数调用开销
- **平铺哈希表**：提升缓存命中率
- **编译器优化**：-O2 -march=native（MinGW 8.1.0 兼容）

---

## 性能对比

测试数据：深交所 000001（平安银行）2022-04-22 全日 L2 逐笔行情，共 **233,875 条消息**。

| 指标 | Python | C++ v1 (-O0) | C++ v2 (-O2) | C++ v2.1 (-O2) | 提升 (v2.1 vs v1) |
|------|--------|--------------|--------------|----------------|-------------------|
| 总耗时 | 56.91 s | 3.62 s | 2.74 s | 1.43 s | **+153%** |
| 吞吐量 | 4109 msg/s | 64613 msg/s | 85368 msg/s | 163904 msg/s | **+153%** |
| p50 延迟 | - | - | 0.3 μs | 0.3 μs | - |
| p99 延迟 | - | - | 71.5 μs | 42.4 μs | **-40.7%** |
| p99.9 延迟 | - | - | 521.8 μs | 143.0 μs | **-72.6%** |
| pmax 延迟 | - | - | 957.3 μs | 475.4 μs | **-50.3%** |
| 重建结果 | ✓ 正确 | ✓ 正确 | ✓ 正确 | ✓ 正确 | 完全一致 |

> 所有版本产生完全相同的订单簿状态：`NumTrades=81049 LastPx=1606 HighPx=1619 LowPx=1540`

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
├── cpp v1/                ← 单线程版本
│   ├── main.cpp           ← C++ v1 入口
│   ├── CMakeLists.txt
│   ├── behave/            ← 订单簿引擎核心
│   └── tool/              ← 消息解析工具
├── cpp v2/                ← 多线程优化版本
│   ├── main.cpp           ← C++ v2 入口
│   ├── CMakeLists.txt
│   ├── core/              ← 基础组件 (SPSC队列、内存池、缓存行、CPU亲和性、延迟统计)
│   ├── pipeline/          ← 管道架构 (生产者/消费者)
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
| C++ 入口 | `cpp v2/orderbook_v2.1.exe` | C++ v2.1 多线程优化版本 |
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
cd "cpp v2" && g++ -std=c++17 -O2 -march=native -DNDEBUG -I. -o orderbook_v2.exe main.cpp pipeline/*.cpp behave/*.cpp tool/msg_util.cpp -lpthread
./orderbook_v2.exe [数据文件路径] [生产者核心] [消费者核心] [队列容量] [批次大小]

# C++ v2.1（推荐，需先编译）
cd "cpp v2" && g++ -std=c++17 -O2 -march=native -DNDEBUG -I. -o orderbook_v2.1.exe main.cpp pipeline/*.cpp behave/*.cpp tool/msg_util.cpp -lpthread
./orderbook_v2.1.exe [数据文件路径] [生产者核心] [消费者核心] [队列容量] [批次大小] [重放次数]
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
- **编译器优化**：-O3 -march=native -flto -DNDEBUG
- **字段重排**：ObOrder 从 40B 压缩到 32B，ObExec 优化到 40B

### CPP v2.1 性能优化
- **HybridLevelBook**：紧凑排序数组（n≤128）+ std::map 回退（n>128），价格档操作提速 3.5x
- **显式模板实例化**：减少编译时间，提升链接效率
- **关键函数强制内联**：减少函数调用开销
- **平铺哈希表**：提升缓存命中率
- **内存预取**：`__builtin_prefetch` 预取下一条消息到 L1 缓存
- **动态批次大小**：一次取空队列，最大 1024 条，适应负载变化

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
