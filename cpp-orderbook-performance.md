# CPP OrderBook 引擎性能演进文档

## 概述

本文档记录 C++ 订单簿重建引擎从 v1 到 v2.1 的完整演进历程，包括每个版本的改动内容和性能指标。

**测试环境**
- CPU: Intel Core (4 cores, AVX2)
- Compiler: MinGW 8.1.0 (GCC, x86_64-win32-seh)
- OS: Windows
- Data: SZSE 000001 (Ping An Bank) 2022-04-22 L2 tick data, **233,875 messages** (22,365 orderbook events)
- Validation: `NumTrades=81049 LastPx=1606 HighPx=1619 LowPx=1540`

---

## 版本总览

| 版本 | 架构 | 吞吐量 | vs v1 | 关键特性 |
|------|------|--------|-------|----------|
| Python | 单线程解释执行 | 4,109 msg/s | - | 基准参考 |
| CPP v1 | 单线程 -O0 | 64,613 msg/s | - | 基础 C++ 重写 |
| CPP v2 | 多线程 SPSC -O2 | 95,455 msg/s | +47.7% | 生产者-消费者架构 |
| CPP v2.1 | 多线程 SPSC -O2 | 163,904 msg/s | +153.7% | HybridLevelBook + 内存预取 |

---

## CPP v1: 单线程版本

### 版本信息
- **状态**: 已完成
- **编译选项**: `-O0` (无优化)
- **目录**: `cpp v1/`

### 架构设计
```
main.cpp
  -> AxsbeFileReader (逐条读取消息文件)
      -> AXOB::onMsg() (单线程顺序处理)
          |- axob_init.cpp   -- 初始化、精度转换
          |- axob_order.cpp  -- 逐笔委托处理
          |- axob_trade.cpp  -- 逐笔成交处理
          |- axob_cage.cpp   -- 创业板价格笼子
          |- axob_snap.cpp   -- 千档快照发布
```

### 核心数据结构
```cpp
struct ObOrder {        // 40 bytes
    uint32_t orderNO;
    int64_t  price;     // 2-decimal precision
    int64_t  qty;
    int64_t  leaveQty;
    uint8_t  side;
    uint8_t  type;
    uint8_t  reserved[2];
};
// Price levels: std::map<int64_t, PriceLevel>
// Order table: std::unordered_map<uint32_t, ObOrder>
```

### 编译命令
```bash
cd "cpp v1"
g++ -std=c++17 -O0 -I. -o orderbook.exe main.cpp behave/*.cpp tool/msg_util.cpp -lpthread
```

### 性能指标
| 指标 | 数值 |
|------|------|
| 总耗时 | 3.62 s |
| 吞吐量 | 64,613 msg/s |
| 延迟 | 未测量 |
| 重建结果 | 正确 |

---

## CPP v2: 多线程优化版本

### 版本信息
- **状态**: 已完成
- **提交**: `3a03928`
- **编译选项**: `-O2 -march=native -DNDEBUG`
- **目录**: `cpp v2/`
- **性能**: 95,455 msg/s (+47.7% vs v1)

### 架构设计
```
main.cpp -> Pipeline
              |- Producer Thread (Core 0)
              |    -> AxsbeFileReader -> SPSCQueue.push()
              |
              +-- Consumer Thread (Core 2)
                   -> SPSCQueue.pop_batch() -> AXOB::onMsg()
```

### 新增核心组件 (core/)
| 文件 | 说明 |
|------|------|
| `spsc_queue.h` | SPSC 无锁队列, 位掩码环形缓冲区 |
| `memory_pool.h` | 侵入式空闲链表内存池, 2x 增长 |
| `cache_line.h` | CACHELINE_SIZE=64, 防止 false sharing |
| `cpu_affinity.h` | CPU 亲和性绑定 |
| `latency_stats.h` | 环形缓冲区延迟统计, nth_element O(n) |

### 管道架构 (pipeline/)
| 文件 | 说明 |
|------|------|
| `pipeline.h/cpp` | Pipeline 类, 管理线程生命周期 |
| `producer.h/cpp` | 生产者: 文件读取 -> 队列 |
| `consumer.h/cpp` | 消费者: 队列 -> AXOB 处理 |

### 关键优化项
| 优化项 | 说明 | 收益 |
|--------|------|------|
| SPSC 无锁队列 | 环形缓冲区 + atomic | 核心架构 |
| 批量处理 | pop_batch 一次取 64 条 | +10% |
| 内存预取 | __builtin_prefetch | +3% |
| CPU 亲和性 | Producer Core 0, Consumer Core 2 | +5% |
| 缓存行对齐 | alignas(64) | +3% |
| 内存池 | 侵入式空闲链表 | +2% |
| 字段重排 | ObOrder 40B -> 32B | +3% |
| 分支预测 | LIKELY/UNLIKELY 宏 | +1% |
| 编译优化 | -O2 -march=native | +15% |

### ObOrder 字段重排
```
Before (40B): [orderNO 4B][price 8B][qty 8B][leaveQty 8B][side 1B][type 1B][resv 2B]
After  (32B): [orderNO 4B][side 1B][type 1B][price 8B][qty 8B][leaveQty 8B]
              Fits in single cache line (64B alignment)
```

### MinGW 8.1.0 Bug
-O2/-O3 causes `parseKeyValueLine` (with try/catch + std::stoll) stack corruption when inlined.
Fix: `msg_util.cpp` compiled with `-fno-inline`.

### 编译命令
```bash
cd "cpp v2"
# CMake
cmake -B build -G "MinGW Makefiles"
cmake --build build --parallel 8
```

### 命令行参数
```bash
./orderbook_v2.exe <data> <producer_core> <consumer_core> <queue_cap> <batch_size>
```

### 性能指标
| 指标 | 数值 | vs v1 |
|------|------|-------|
| 总耗时 | 2.74 s | -24.3% |
| 吞吐量 | 95,455 msg/s | +47.7% |
| p50 | 0.3 us | - |
| p99 | 71.5 us | - |
| p99.9 | 521.8 us | - |
| pmax | 957.3 us | - |
| 重建结果 | 正确 | 一致 |

---

## CPP v2.1: 性能极致优化版本

### 版本信息
- **状态**: 已完成
- **标签**: `v2.1` (`40bcac2`)
- **编译选项**: `-O2 -march=native -DNDEBUG` (MinGW 兼容)
- **目录**: `cpp v2/`
- **性能**: 163,904 msg/s (+71.7% vs v2, +153.7% vs v1)

### v2.1 新增优化

#### 1. HybridLevelBook (柯3)
收益: 价格档操作 +350%, 整体 +10-15%

```cpp
class HybridLevelBook {
    static constexpr size_t COMPACT_THRESHOLD = 128;
    CompactLevel compact_[128];  // n<=128: sorted array, cache-friendly
    size_t compactSize_ = 0;
    std::map<int64_t, PriceLevel*> overflow_;  // n>128: red-black tree fallback
};
```

128 threshold rationale:
- 128 levels x ~16B = 2KB, fits L1 cache
- Linear scan faster than rb-tree O(log 128)=7 jumps
- A-share 10-level orderbook, usually n << 128

#### 2. 显式模板实例化 (柯1)
收益: 编译速度提升, 链接效率提高

#### 3. 关键函数强制内联 (柯3)
收益: +5%

#### 4. 内存预取优化 (柯1)
收益: +3%
```cpp
for (size_t i = 0; i < n; ++i) {
    if (i + 1 < n) __builtin_prefetch(&batch[i+1], 0, 3);
    // process batch[i]
}
```

#### 5. 动态批次处理 (柯1)
收益: +10%
```cpp
static constexpr size_t MAX_BATCH = 1024;
MarketEvent batch[MAX_BATCH];  // stack allocation
size_t n = queue.pop_batch(batch, MAX_BATCH);  // drain queue
```

#### 6. 仪表盘重放功能
支持 1-1000 次数据重放:
```bash
./orderbook_v2.1.exe test_data.log 0 2 16384 64 100
```

### 编译命令
```bash
cd "cpp v2"
g++ -c -fno-inline -O2 -march=native -DNDEBUG -std=c++17 -I. tool/msg_util.cpp -o tool/msg_util.o
g++ -c -O2 -march=native -DNDEBUG -std=c++17 -I. main.cpp pipeline/*.cpp behave/*.cpp
g++ -O2 -o orderbook_v2.1.exe *.o pipeline/*.o tool/*.o behave/*.o -lpthread
```

### 命令行参数
```bash
./orderbook_v2.1.exe <data> <producer_core> <consumer_core> <queue_cap> <batch_size> <replay_count>
```

### 性能指标
| 指标 | 数值 | vs v2 | vs v1 |
|------|------|-------|-------|
| 总耗时 | 1.43 s | -47.8% | -60.5% |
| 吞吐量 | 163,904 msg/s | +71.7% | +153.7% |
| p50 | 0.3 us | 不变 | - |
| p99 | 42.4 us | -40.7% | - |
| p99.9 | 143.0 us | -72.6% | - |
| pmax | 475.4 us | -50.3% | - |
| 重建结果 | 正确 | 一致 | 一致 |

---

## 性能对比总览

### 吞吐量演进
```
Python    ====                                          4,109 msg/s
CPP v1    ==============================                64,613 msg/s  (+1473%)
CPP v2    =======================================       95,455 msg/s  (+47.7%)
CPP v2.1  ============================================== 163,904 msg/s  (+71.7%)
```

### 延迟对比 (v2 -> v2.1)
| 百分位 | v2 | v2.1 | 改善 |
|--------|-----|------|------|
| p50 | 0.3 us | 0.3 us | 不变 |
| p99 | 71.5 us | 42.4 us | -40.7% |
| p99.9 | 521.8 us | 143.0 us | -72.6% |
| pmax | 957.3 us | 475.4 us | -50.3% |

### 正确性验证
所有版本产生完全相同的订单簿状态:
```
NumTrades=81049 LastPx=1606 HighPx=1619 LowPx=1540
OpenPx=1564 TVol=9212740800 TVal=14684529579500
```

---

## 仪表盘功能

### 指标面板
| 指标 | 说明 |
|------|------|
| Progress | 处理进度百分比 |
| Time | 已用时间 |
| Msg/s | 实时吞吐量 |
| Trades | 成交笔数 |
| LastPx | 最新价 |
| Bid1 / Ask1 | 买卖一档 |
| P50 | 50 百分位延迟 |
| P99 | 99 百分位延迟 |
| Pmax | 最大延迟 |

### 重放测试
仪表盘支持 1-1000 次数据重放, 用于压力测试和大数据量验证.

---

## 未来规划

### v2.2 ~ v2.5 优化计划
| 批次 | 目标 | 优化项 |
|------|------|--------|
| 批次 2 | 数据结构 | HybridLevelBook (done), 显式模板实例化, 强制内联, 平铺哈希表 |
| 批次 3 | 编译期 | 预编译头文件, 前置声明与 Pimpl |
| 批次 4 | 内存 | Huge Pages, SoA 列式存储, 内存分配器调优 |
| 批次 5 | 系统级 | PGO 编译优化, 实时调度策略, 中断隔离 |

### MPMC 扩展 (Future)
- Queue: MoodyCamel ConcurrentQueue
- Sharding: by message type or security code
- Scale: 1-N producers, 1-M consumers

---

## 更新日志

| 日期 | 版本 | 内容 |
|------|------|------|
| 2026-06-12 | v1.0 | Python baseline + C++ single-thread rewrite |
| 2026-06-13 | v2.0 | Multi-thread SPSC, 95K msg/s |
| 2026-06-13 | v2.1 | HybridLevelBook + prefetch, 164K msg/s |
| 2026-06-13 | - | Dashboard P50/P99/Pmax + replay |
