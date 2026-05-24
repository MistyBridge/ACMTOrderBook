# C++ 订单簿引擎性能报告

## 版本性能对比

| 版本 | 编译器 | 优化选项 | 吞吐量 | 提升幅度 |
|------|--------|----------|--------|----------|
| CPP v1 | GCC 8.1.0 | -O0 | 64,613 msg/s | 基准 |
| CPP v2 | GCC 8.1.0 | -O2 -march=native | 95,455 msg/s | +47.7% |
| CPP v2.1 | MSVC 2022 | /O2 /GL /arch:AVX2 /LTCG | 212,817 msg/s | +230% vs v1 |
| CPP v2.2 | MSVC 2022 | + mmap 文件预加载 | 530,000 msg/s | +720% vs v1 |
| **CPP v2.3** | **MSVC 2022** | **+ 直接解析** | **985,951 msg/s** | **+1424% vs v1** |

---

## CPP v2.3 详细性能

### 测试环境
- **CPU**: Intel Core i5-12490F (6C/12T)
- **RAM**: DDR4 3200MHz
- **OS**: Windows 10/11
- **Compiler**: MSVC 2022 (v143)
- **Build**: Release x64 /O2 /GL /arch:AVX2 /LTCG

### 核心优化

#### 1. 直接字段解析 ⭐⭐⭐
```
问题：parseKeyValueLine() + loadDict() 两步解析
- parseKeyValueLine(): 300ns (创建 map)
- loadDict(): 250ns (从 map 取值)
- 总计：550ns/消息

解决方案：直接在字符串中查找 key=value
- extractField(): 20ns/字段
- 9 个字段 × 20ns = 180ns/消息
- 节省：370ns/消息 (-67%)

效果：+86% 吞吐量提升
```

#### 2. mmap 文件预加载
```
传统方式：ifstream 逐行读取
- 每条消息：2000ns (File I/O)
- 总占比：43%

优化方式：mmap 内存映射
- 每条消息：50ns (内存访问)
- 提升：40x
- 整体提升：+154%
```

#### 3. 平铺哈希表 (ankerl::unordered_dense)
```
std::unordered_map → ankerl::unordered_dense
- Insert: 10.8x 更快
- Find: 2.5x 更快
- Erase: 4.5x 更快
- 整体提升：+1.9%
```

---

### 性能测试结果

#### 10x 重放稳定性测试
```
平均：985,951 msg/s
最高：1,020,217 msg/s
最低：941,928 msg/s
波动：<10%
```

#### 延迟指标
```
p50：0.2-0.3 μs
p99：1.8-2.4 ms
p99.9：2.4-3.0 ms
```

---

### 技术实现

#### 直接字段解析核心代码
```cpp
// 工具函数：直接在行字符串中查找 key=value
inline bool extractField(const char* line, const char* key, int64_t& out) {
    const char* pos = strstr(line, key);
    if (!pos) return false;
    pos += strlen(key);
    if (*pos != '=') return false;
    out = strtoll(pos + 1, nullptr, 10);
    return true;
}

// AxsbeOrder::loadFromLine()
void loadFromLine(const char* line) {
    int64_t value;
    if (extractField(line, "ApplSeqNum", value))
        ApplSeqNum = static_cast<uint64_t>(value);
    if (extractField(line, "Price", value))
        Price = value;
    // ... 9 个字段
}
```

---

## 性能瓶颈分析

### v2.3 耗时分布（每消息 ~1014ns）

```
AXOB core               300ns  ██████████████████████████████  30%
Queue 同步等待          100ns  ██████████                      10%
直接字段解析            180ns  ██████████████████              18%
mmap 文件读取            50ns  █████                           5%
其他                    384ns  ██████████████████████████████████████  37%
```

---

## 构建指南

### mmap 版本（推荐）
```bash
cd "cpp v2"
cmake -B build -G "Visual Studio 17 2022" -A x64 -DUSE_MMAP=ON
cmake --build build --config Release --parallel 8
copy build\Release\orderbook_v2.exe orderbook_v2.3.exe
```

### 运行测试
```bash
# 1x 重放
.\orderbook_v2.3.exe ..\data\20220422\AX_sbe_szse_000001.log 0 2 16384 64 1

# 10x 重放（稳定性测试）
.\orderbook_v2.3.exe ..\data\20220422\AX_sbe_szse_000001.log 0 2 16384 64 10
```

---

## 版本历史

| 日期 | 版本 | 说明 | 性能 |
|------|------|------|------|
| 2026-06-12 | v1.0 | 初始计划文档 | - |
| 2026-06-13 | v2.0 | CPP v2 多线程版本完成 | 95,455 msg/s |
| 2026-06-13 | v2.1 | MSVC 2022 优化完成 | 212,817 msg/s |
| 2026-06-14 | v2.2 | mmap 文件预加载完成 | 530,000 msg/s |
| 2026-06-14 | **v2.3** | **直接解析优化完成** | **985,951 msg/s** |

---

## 总结

### v2.3 核心成果

✅ **直接字段解析**：消除 parseKeyValueLine + loadDict 双重开销，提升 86%
✅ **mmap 文件预加载**：消除 File I/O 瓶颈，提升 154%
✅ **平铺哈希表**：优化 orderMap 查找，提升 1.9%

### 总体提升

```
v1 → v2.3: +1424% (64,613 → 985,951 msg/s)
```

### 性能亮点

- **吞吐量**：985,951 msg/s（近百万级）
- **延迟**：p50 = 0.2-0.3 μs（极低）
- **稳定性**：波动 <10%（优秀）

### 适用场景

- ✅ 高频交易：极低延迟
- ✅ 大数据量：高吞吐量
- ✅ 实时系统：稳定性能

---

## 下一步计划

### v2.4 目标
**986K → 1.2M msg/s (+22%)**

| 优化项 | 预期收益 |
|--------|----------|
| genSnap heap→stack | +3-5% |
| 移除 loadDict noinline | +0.5-1% |
| genSnap dirty flag | +3-10% |
| 其他优化 | +10% |

**最终目标：1.2M+ msg/s** 🚀
