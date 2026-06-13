#pragma once
// =====================================================================
//  core/cpu_affinity.h — CPU 亲和性绑定工具
//
//  跨平台（Windows / Linux / macOS）的线程-核心绑定与物理核心数查询。
//  失败时返回 false 并打印警告，不抛出异常。
//
//  接口：
//    setThreadAffinity(std::thread& t, int coreId)  — 需要 <thread> 支持
//    setThreadAffinityByHandle(HANDLE h, int coreId) — Windows 原生句柄版本
//    setThreadAffinityByNative(pthread_t h, int coreId) — POSIX 线程 ID 版本
//    getPhysicalCoreCount()                           — 返回物理核心数
//
//  用法示例：
//    std::thread producer(producerFunc);
//    std::thread consumer(consumerFunc);
//    setThreadAffinity(producer, 0);  // 绑定到核心 0
//    setThreadAffinity(consumer, 2);  // 绑定到核心 2（不同物理核）
// =====================================================================

#include <cstdio>
#include <cstddef>
#include <vector>

// =====================================================================
//  检测 std::thread 可用性
//  MSVC / GCC posix 线程模型 / Clang：通常可用
//  MinGW win32 线程模型：不可用
// =====================================================================
#if defined(__has_include)
    #if __has_include(<thread>) && (!defined(__MINGW32__) || defined(_GLIBCXX_HAS_GTHREADS))
        #include <thread>
        #define AXOB_HAS_STD_THREAD 1
    #endif
#elif !defined(__MINGW32__)
    #include <thread>
    #define AXOB_HAS_STD_THREAD 1
#endif

// =====================================================================
//  平台检测与头文件引入
// =====================================================================
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

#elif defined(__APPLE__)
    #include <sys/sysctl.h>
    #include <pthread.h>
    #include <mach/mach.h>
    #include <mach/thread_policy.h>

#elif defined(__linux__)
    #include <pthread.h>
    #include <unistd.h>
    #include <fstream>
    #include <string>
    #include <set>

#else
    #warning "cpu_affinity.h: unsupported platform, affinity functions will be stubs"
#endif

namespace axob {
namespace core {

// =====================================================================
//  内部：校验 coreId 有效性
// =====================================================================
namespace detail {
inline bool checkCoreId(int coreId) {
    if (coreId < 0) {
        std::fprintf(stderr, "[cpu_affinity] Warning: invalid coreId %d\n", coreId);
        return false;
    }
    return true;
}
}  // namespace detail

// =====================================================================
//  setThreadAffinity(std::thread&, coreId)
//  需要 std::thread 支持（MSVC / GCC posix 线程模型）
// =====================================================================
#ifdef AXOB_HAS_STD_THREAD
inline bool setThreadAffinity(std::thread& t, int coreId) {
    if (!detail::checkCoreId(coreId)) return false;

#if defined(_WIN32)
    HANDLE hThread = static_cast<HANDLE>(t.native_handle());
    if (hThread == nullptr) {
        std::fprintf(stderr, "[cpu_affinity] Warning: null thread handle\n");
        return false;
    }
    DWORD_PTR mask = DWORD_PTR(1) << coreId;
    DWORD_PTR prev = SetThreadAffinityMask(hThread, mask);
    if (prev == 0) {
        std::fprintf(stderr, "[cpu_affinity] Warning: SetThreadAffinityMask failed "
                     "(core %d, error %lu)\n", coreId, GetLastError());
        return false;
    }
    return true;

#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::fprintf(stderr, "[cpu_affinity] Warning: pthread_setaffinity_np failed "
                     "(core %d, errno %d)\n", coreId, rc);
        return false;
    }
    return true;

#elif defined(__APPLE__)
    mach_port_t machThread = pthread_mach_thread_np(t.native_handle());
    struct thread_affinity_policy policy;
    policy.affinity_tag = static_cast<integer_t>(coreId + 1);
    kern_return_t kr = thread_policy_set(
        machThread, THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_AFFINITY_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        std::fprintf(stderr, "[cpu_affinity] Warning: thread_policy_set failed "
                     "(core %d, kr=%d). macOS only supports soft affinity.\n", coreId, kr);
        return false;
    }
    return true;

#else
    (void)t; (void)coreId;
    std::fprintf(stderr, "[cpu_affinity] Warning: setThreadAffinity not implemented\n");
    return false;
#endif
}
#endif  // AXOB_HAS_STD_THREAD

// =====================================================================
//  setThreadAffinityByHandle(HANDLE, coreId)  —— Windows 原生版本
//  当 std::thread 不可用时（如 MinGW win32 线程模型），使用此接口。
// =====================================================================
#if defined(_WIN32)
inline bool setThreadAffinityByHandle(HANDLE hThread, int coreId) {
    if (!detail::checkCoreId(coreId)) return false;
    if (hThread == nullptr) {
        std::fprintf(stderr, "[cpu_affinity] Warning: null thread handle\n");
        return false;
    }
    DWORD_PTR mask = DWORD_PTR(1) << coreId;
    DWORD_PTR prev = SetThreadAffinityMask(hThread, mask);
    if (prev == 0) {
        std::fprintf(stderr, "[cpu_affinity] Warning: SetThreadAffinityMask failed "
                     "(core %d, error %lu)\n", coreId, GetLastError());
        return false;
    }
    return true;
}
#endif  // _WIN32

// =====================================================================
//  setThreadAffinityByNative(pthread_t, coreId)  —— POSIX 原生版本
// =====================================================================
#if defined(__linux__) || defined(__APPLE__)
inline bool setThreadAffinityByNative(pthread_t tid, int coreId) {
    if (!detail::checkCoreId(coreId)) return false;

#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    int rc = pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::fprintf(stderr, "[cpu_affinity] Warning: pthread_setaffinity_np failed "
                     "(core %d, errno %d)\n", coreId, rc);
        return false;
    }
    return true;

#elif defined(__APPLE__)
    mach_port_t machThread = pthread_mach_thread_np(tid);
    struct thread_affinity_policy policy;
    policy.affinity_tag = static_cast<integer_t>(coreId + 1);
    kern_return_t kr = thread_policy_set(
        machThread, THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_AFFINITY_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        std::fprintf(stderr, "[cpu_affinity] Warning: thread_policy_set failed "
                     "(core %d, kr=%d)\n", coreId, kr);
        return false;
    }
    return true;
#endif
}
#endif  // __linux__ || __APPLE__

// =====================================================================
//  getPhysicalCoreCount()
//  返回系统物理 CPU 核心数（不含超线程的逻辑核心）。
//  若无法确定，退化返回逻辑处理器数。
// =====================================================================
inline int getPhysicalCoreCount() {

#if defined(_WIN32)
    // GetLogicalProcessorInformation 遍历 RelationProcessorCore 计数物理核心
    DWORD bufLen = 0;
    GetLogicalProcessorInformation(nullptr, &bufLen);
    if (bufLen == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<int>(si.dwNumberOfProcessors);
    }

    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
        bufLen / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(info.data(), &bufLen)) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<int>(si.dwNumberOfProcessors);
    }

    int cores = 0;
    for (const auto& entry : info) {
        if (entry.Relationship == RelationProcessorCore) {
            ++cores;
        }
    }
    return cores > 0 ? cores : 1;

#elif defined(__APPLE__)
    int cores = 0;
    size_t size = sizeof(cores);
    if (sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) != 0) {
        int logical = 0;
        size = sizeof(logical);
        if (sysctlbyname("hw.logicalcpu", &logical, &size, nullptr, 0) == 0) {
            return logical;
        }
        return 1;
    }
    return cores > 0 ? cores : 1;

#elif defined(__linux__)
    // 统计 /sys 中唯一的 (physical_package_id, core_id) 组合
    {
        int maxCpu = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
        std::set<std::string> uniqueCores;
        for (int i = 0; i < maxCpu; ++i) {
            char path[256];
            std::snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
            std::ifstream fPkg(path);
            if (!fPkg.is_open()) continue;
            std::string pkgId;
            std::getline(fPkg, pkgId);

            std::snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
            std::ifstream fCore(path);
            if (!fCore.is_open()) continue;
            std::string coreIdStr;
            std::getline(fCore, coreIdStr);

            uniqueCores.insert(pkgId + ":" + coreIdStr);
        }
        if (!uniqueCores.empty()) {
            return static_cast<int>(uniqueCores.size());
        }
    }
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));

#else
    return 1;  // 未知平台保守返回 1
#endif
}

}  // namespace core
}  // namespace axob