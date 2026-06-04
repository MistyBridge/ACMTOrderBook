#include <cstdio>
#include <intrin.h>

bool hasSSE42() {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1 << 20)) != 0;
}

bool hasAVX() {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1 << 28)) != 0;
}

bool hasAVX2() {
    int cpuInfo[4];
    __cpuid(cpuInfo, 7);
    return (cpuInfo[1] & (1 << 5)) != 0;
}

int main() {
    printf("CPU SIMD Support:\n");
    printf("  SSE4.2: %s\n", hasSSE42() ? "Yes" : "No");
    printf("  AVX:    %s\n", hasAVX() ? "Yes" : "No");
    printf("  AVX2:   %s\n", hasAVX2() ? "Yes" : "No");
    return 0;
}
