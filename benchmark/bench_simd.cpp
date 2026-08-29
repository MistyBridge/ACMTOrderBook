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
    std::vector<std::string> lines;
    // 生成一批模拟逐笔行 (深市 SZSE MsgType=192), 字段顺序固定
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

    printf("AVX2 support: %s, SSE4.2: %s\n", hasAVX2()?"YES":"no", hasSSE42()?"YES":"no");
    printf("lines=%d\n", (int)lines.size());

    auto bench = [&](const char* name, const char* (*fn)(const char*, const char*), bool enabled) {
        if (!enabled) { printf("%-8s disabled\n", name); return; }
        // correctness smoke
        int found = 0;
        for (auto& s : lines) if (fn(s.c_str(), "ApplSeqNum")) found++;
        auto t0 = std::chrono::high_resolution_clock::now();
        volatile uint64_t sink = 0;
        for (int rep = 0; rep < 5; ++rep) {
            uint64_t acc = 0;
            for (auto& s : lines) {
                const char* p1 = fn(s.c_str(), "ApplSeqNum");
                const char* p2 = fn(s.c_str(), "Price");
                const char* p3 = fn(s.c_str(), "OrderQty");
                if (p1) acc += (uint64_t)(p1[10]);   // 防优化
                if (p2) acc += (uint64_t)(p2[5]);
                if (p3) acc += (uint64_t)(p3[8]);
            }
            sink += acc;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double callsPerMs = (double)((uint64_t)N * 3 * 5) / ms;
        printf("%-8s found=%d  %6.1f ms  %8.0f field-scans/ms\n", name, found, ms, callsPerMs);
        (void)sink;
    };

    bench("scalar", find_scalar, true);
    bench("sse4.2", find_sse, hasSSE42());
    bench("avx2",   find_avx2, hasAVX2());
    return 0;
}
