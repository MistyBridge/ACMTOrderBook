// bench_simd.cpp — A/B 对比: scalar / SSE4.2 / AVX2 三种字段解析(Key=Value 扫描)吞吐。
//
// 用一组模拟的 //MsgType=... 逐笔行情行, 对每个实现统计能找到的字段数(正确性)与总耗时。
// 结论用于决定 AVX2 是否值得接进生产热路径 (本项目 field_parser.h 已注明 SIMD 曾回滚)。
//
// 编译 (MSVC, 支持 AVX2):
//   cl /O2 /std:c++17 /arch:AVX2 /utf-8 bench_simd.cpp
// 编译 (GCC/Clang):
//   g++ -O2 -mavx2 -std=c++17 bench_simd.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <chrono>
#include <intrin.h>

// ---- 运行时 CPU 能力探测 ----
static bool hasSSE42() {
    int cpu[4]; __cpuid(cpu, 1);
    return (cpu[2] & (1 << 20)) != 0;
}
static bool hasAVX2() {
    int cpu[4]; __cpuid(cpu, 0);
    if (cpu[0] < 7) return false;
    __cpuid(cpu, 7);
    return (cpu[1] & (1 << 5)) != 0;  // EBX bit5 = AVX2
}
static int ctz32(unsigned v) { unsigned long i; _BitScanForward(&i, v); return (int)i; }

// ---- scalar: 标准 strstr (当前生产路径 extractField 用) ----
static const char* find_scalar(const char* hay, const char* needle) {
    return strstr(hay, needle);
}
// ---- SSE4.2: _mm_cmpistri (strstr_simd 路径) ----
static const char* find_sse(const char* hay, const char* needle) {
    const char* first = needle;
    __m128i f = _mm_set1_epi8(first[0]);
    size_t nl = strlen(needle), hl = strlen(hay);
    if (nl > hl) return nullptr;
    const char* end = hay + hl - nl + 1;
    const char* p = hay;
    while (p < end) {
        __m128i block = _mm_loadu_si128((const __m128i*)p);
        int mask = _mm_cmpistri(f, block, _SIDD_CMP_EQUAL_ANY | _SIDD_UBYTE_OPS);
        if (mask < 16) {
            const char* cand = p + mask;
            if (cand + nl <= hay + hl && strncmp(cand, needle, nl) == 0) return cand;
            p = cand + 1;
        } else p += 16;
    }
    return nullptr;
}
// ---- AVX2: _mm256_cmpeq + movemask (一次 32 字节) ----
static const char* find_avx2(const char* hay, const char* needle) {
    const char* first = needle;
    __m256i f = _mm256_set1_epi8(first[0]);
    size_t nl = strlen(needle), hl = strlen(hay);
    if (nl > hl) return nullptr;
    const char* end = hay + hl - nl + 1;
    const char* p = hay;
    while (p < end) {
        __m256i block = _mm256_loadu_si256((const __m256i*)p);
        __m256i cmp = _mm256_cmpeq_epi8(block, f);
        int mask = _mm256_movemask_epi8(cmp);
        while (mask) {
            int idx = ctz32((unsigned)mask);
            const char* cand = p + idx;
            if (cand + nl <= hay + hl && strncmp(cand, needle, nl) == 0) return cand;
            mask &= mask - 1;
        }
        p += 32;
    }
    return nullptr;
}

int main() {
    // 场景A: L2 逐笔行 (短, ~150B) —— 生产实际负载
    std::vector<std::string> lines;
    int N = 260000;
    lines.reserve(N);
    for (int i = 0; i < N; ++i) {
        char buf[160];
        int px = 99800 + (i % 400) * 100;
        int qty = 1000 + (i % 99) * 100;
        snprintf(buf, sizeof(buf),
            "//MsgType=192 SecurityIDSource=102 SecurityID=300001 ApplSeqNum=%d "
            "Price=%d OrderQty=%d Side=1 OrdType=2 TransactTime=2022042210%d",
            i, px, qty, 93000000 + (i % 1000000));
        lines.emplace_back(buf);
    }

    // 场景B: 长文本 (~2KB) —— 理论上是 SIMD 的强项场景 (非 L2 生产负载)
    std::vector<std::string> longlines;
    longlines.reserve(30000);
    for (int i = 0; i < 30000; ++i) {
        std::string s = "//MsgType=192 SecurityIDSource=102 SecurityID=300001 ApplSeqNum=";
        s += std::to_string(i) + " Price=99800 OrderQty=1000 Side=1 OrdType=2 ";
        s += std::string(1800, 'x');  // 填充长内容
        s += " TransactTime=20220422100000000";
        longlines.emplace_back(s);
    }

    printf("AVX2 support: %s, SSE4.2: %s\n", hasAVX2()?"YES":"no", hasSSE42()?"YES":"no");

    // best-of-3 测量: 返回 field-scans/ms
    auto bench = [&](const char* name, const char* (*fn)(const char*, const char*),
                     const std::vector<std::string>& corpus, bool enabled) {
        if (!enabled) { printf("  %-8s(%-8s) disabled\n", name, corpus[0].size()>200?"long":"short"); return; }
        // 正确性等价: 三种实现找到的匹配数必须一致
        uint64_t foundA=0, foundB=0;
        for (auto& s : corpus) { if (fn(s.c_str(), "ApplSeqNum")) foundA++;  if (fn(s.c_str(), "SecurityID")) foundB++; }
        double best = 1e18;
        for (int rep = 0; rep < 3; ++rep) {
            auto t0 = std::chrono::high_resolution_clock::now();
            uint64_t acc = 0;
            for (auto& s : corpus) {
                const char* p1 = fn(s.c_str(), "ApplSeqNum");
                const char* p2 = fn(s.c_str(), "Price");
                const char* p3 = fn(s.c_str(), "OrderQty");
                if (p1) acc += (uint64_t)(p1[10]); if (p2) acc += (uint64_t)(p2[5]); if (p3) acc += (uint64_t)(p3[8]);
            }
            // 防优化: 编译器无法删除核心循环; (volatile) 写回
            *(volatile uint64_t*)&acc;
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (ms < best) best = ms;
        }
        double scansPerMs = (double)(corpus.size() * 3) / best;
        printf("  %-8s(%-8s) best %6.2f ms  %9.0f field-scans/ms  (foundA=%llu foundB=%llu)\n",
               name, corpus[0].size()>200?"long":"short", best, scansPerMs,
               (unsigned long long)foundA, (unsigned long long)foundB);
    };

    printf("--- 场景A: 短 L2 行 (生产负载) ---\n");
    bench("scalar", find_scalar, lines, true);
    bench("sse4.2", find_sse, lines, hasSSE42());
    bench("avx2",   find_avx2, lines, hasAVX2());
    printf("--- 场景B: 长文本 (~2KB, 非生产负载, 理论上是 SIMD 强项) ---\n");
    bench("scalar", find_scalar, longlines, true);
    bench("sse4.2", find_sse, longlines, hasSSE42());
    bench("avx2",   find_avx2, longlines, hasAVX2());
    return 0;
}
