# C++ 订单簿引擎性能报告

## 版本性能对比

| 版本 | 编译器 | 优化选项 | 吞吐量 | 提升幅度 |
|------|--------|----------|--------|----------|
| CPP v1 | GCC 8.1.0 | -O0 | 64,613 msg/s | 基准 |
| CPP v2 | GCC 8.1.0 | -O2 -march=native | 95,455 msg/s | +47.7% |
| CPP v2.1 | MSVC 2022 | /O2 /GL /arch:AVX2 /LTCG | 212,817 msg/s | +198% vs v1 |
| **CPP v2.2** | **MSVC 2022** | **/O2 /GL /arch:AVX2 /LTCG + mmap** | **540,000 msg/s** | **+737% vs v1** |

---

## CPP v2.2 详细性能

### 测试环境
- **CPU**: Intel Core i5-12490F (6C/12T)
- **RAM**: DDR4 3200MHz
- **OS**: Windows 10/11
- **Compiler**: MSVC 2022 (v143)
- **Build**: Release x64 /O2 /GL /arch:AVX2 /LTCG

### 核心优化

#### 1. mmap 文件预加载 ⭐
```
传统方式：ifstream 逐行读取
- 每条消息：2000ns (File I/O)
- 总占比：43%

优化方式：mmap 内存映射
- 每条消息：50ns (内存访问)
- 提升：40x
- 整体提升：+154%
```

#### 2. 平铺哈希表 (ankerl::unordered_dense)
```
std::unordered_map → ankerl::unordered_dense
- Insert: 10.8x 更快
- Find: 2.5x 更快
- Erase: 4.5x 更快
- 整体提升：+1.9%
```

#### 3. O(1) 最优价优化
```cpp
// 优化前：O(n) 线性扫描
for (int i = bidLevelBook.count - 1; i >= 0; i--) {
    if (bidLevelBook.levels[i].price < price) { ... break; }
}

// 优化后：O(1) 直接取
if (bidLevelBook.count > 0) {
    bidMaxPrice = bidLevelBook.levels[bidLevelBook.count - 1].price;
}
```

### 性能测试结果

#### 1x 重放测试
```
消息总数: 233,875
- 委托消息: 122,359
- 成交消息: 106,434
- 快照消息: 5,082

执行时间: 0.466s
吞吐量: 501,630 msg/s
```

#### 10x 重放稳定性测试
```
Run 1: 4.333s (539,713 msg/s)
Run 2: 4.330s (540,071 msg/s)
Run 3: 4.256s (549,504 msg/s)

平均: 543,096 msg/s
稳定性: ✅ 优秀
```

---

## 性能瓶颈分析

### v2.2 耗时分布（每消息 ~1850ns）

```
Queue 同步等待        500ns  ██████████████████████████████  27%
parseKeyValueLine     300ns  ██████████████████              16%
AXOB core             300ns  ██████████████████              16%
genSnap                70ns  ████                            4%
延迟测量               70ns  ████                            4%
其他                  610ns  ██████████████████████████████████████  33%
```

### 下一步优化方向

1. **parseKeyValueLine 优化** (-200ns)
   - string_view 零拷贝
   - 预期提升：+7%

2. **热路径函数内联** (-50ns)
   - HybridLevelBook::find()
   - 预期提升：+2%

3. **Queue 同步优化** (-200ns)
   - 批量处理
   - 预期提升：+7%

---

## 构建指南

### mmap 版本（推荐）
```bash
cd "cpp v2"
cmake -B build -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=ON
cmake --build build --config Release --parallel 8
copy build\Release\orderbook_v2.exe orderbook_v2.2.exe
```

### 传统版本
```bash
cd "cpp v2"
cmake -B build -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=OFF
cmake --build build --config Release --parallel 8
copy build\Release\orderbook_v2.exe orderbook_v2.2.exe
```

### 运行测试
```bash
# 1x 重放
.\orderbook_v2.2.exe ..\data\20220422\AX_sbe_szse_000001.log 0 2 16384 64 1

# 10x 重放（稳定性测试）
.\orderbook_v2.2.exe ..\data\20220422\AX_sbe_szse_000001.log 0 2 16384 64 10
```

---

## 版本历史

| 日期 | 版本 | 说明 | 性能 |
|------|------|------|------|
| 2026-06-12 | v1.0 | 初始计划文档 | - |
| 2026-06-13 | v2.0 | CPP v2 多线程版本完成 | 95,455 msg/s |
| 2026-06-13 | v2.1 | MSVC 2022 优化完成 | 212,817 msg/s |
| 2026-06-14 | v2.2 | mmap 文件预加载完成 | **540,000 msg/s** |

---

## 总结

### v2.2 核心成果

✅ **mmap 文件预加载**：消除 File I/O 瓶颈，提升 154%  
✅ **平铺哈希表**：优化 orderMap 查找，提升 1.9%  
✅ **O(1) 最优价**：优化 HybridLevelBook，提升 0.5%  

### 总体提升

```
v1 → v2.2: +737% (64,613 → 540,000 msg/s)
```

### 下一步目标

```
v2.2 → v2.3: +7% (540,000 → 580,000 msg/s)
v2.3 → v2.4: +3% (580,000 → 600,000 msg/s)
v2.4 → v2.5: +8% (600,000 → 650,000 msg/s)
```

**最终目标：650K+ msg/s** 🚀
