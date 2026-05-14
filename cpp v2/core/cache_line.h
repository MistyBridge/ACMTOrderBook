#pragma once
// =====================================================================
//  core/cache_line.h — 缓存行对齐工具
//
//  提供 CACHELINE_SIZE 常量、CacheLinePadded<T> 包裹模板、
//  CacheLineAligned<T> 对齐模板以及 CACHELINE_PAD 宏，
//  用于消除多线程环境中的伪共享 (false sharing)。
//
//  用法示例：
//    struct SharedState {
//        CacheLinePadded<std::atomic<uint64_t>> writeIndex;  // 生产者写
//        CacheLinePadded<std::atomic<uint64_t>> readIndex;   // 消费者读
//    };
//    // writeIndex 与 readIndex 各占独立 cache line，互不干扰
// =====================================================================

#include <cstddef>
#include <cstdint>
#include <new>        // std::hardware_destructive_interference_size
#include <type_traits>

namespace axob {
namespace core {

// -----------------------------------------------------------------
//  缓存行大小常量
// -----------------------------------------------------------------
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr size_t CACHELINE_SIZE = std::hardware_destructive_interference_size;
#else
    inline constexpr size_t CACHELINE_SIZE = 64;  // x86-64 / ARM 的典型值
#endif

// -----------------------------------------------------------------
//  CacheLinePadded<T>
//  将 T 包裹在一个独占整条 cache line 的结构体中。
//  相邻的 CacheLinePadded 实例保证不会落在同一条 cache line 上。
// -----------------------------------------------------------------
template <typename T>
struct alignas(CACHELINE_SIZE) CacheLinePadded {
    T value;

    CacheLinePadded() noexcept(std::is_nothrow_default_constructible_v<T>) : value{} {}

    // 拷贝/移动构造仅在 T 支持时启用（std::atomic 等不可拷贝类型走默认构造）
    template <typename U = T,
              typename = std::enable_if_t<std::is_copy_constructible_v<U>>>
    explicit CacheLinePadded(const T& v)
        noexcept(std::is_nothrow_copy_constructible_v<T>) : value(v) {}

    template <typename U = T,
              typename = std::enable_if_t<std::is_move_constructible_v<U>>>
    explicit CacheLinePadded(T&& v)
        noexcept(std::is_nothrow_move_constructible_v<T>)
        : value(static_cast<T&&>(v)) {}

    // 隐式转换 —— 注意：对成员使用 "." 访问时隐式转换不生效，
    // 需要通过 .value 访问底层对象（如 .value.load()）。
    operator T&()              noexcept { return value; }
    operator const T&() const  noexcept { return value; }

    CacheLinePadded& operator=(const T& v)
        noexcept(std::is_nothrow_copy_assignable_v<T>) { value = v; return *this; }
    CacheLinePadded& operator=(T&& v)
        noexcept(std::is_nothrow_move_assignable_v<T>) { value = static_cast<T&&>(v); return *this; }
};

// -----------------------------------------------------------------
//  CacheLineAligned<T>
//  将 T 对齐到 cache line 边界，但不保证独占整条 cache line。
//  适用于数组元素或已知不会与其他线程共享的场景。
// -----------------------------------------------------------------
template <typename T>
struct alignas(CACHELINE_SIZE) CacheLineAligned {
    T value;

    CacheLineAligned() noexcept(std::is_nothrow_default_constructible_v<T>) : value{} {}

    template <typename U = T,
              typename = std::enable_if_t<std::is_copy_constructible_v<U>>>
    explicit CacheLineAligned(const T& v)
        noexcept(std::is_nothrow_copy_constructible_v<T>) : value(v) {}

    template <typename U = T,
              typename = std::enable_if_t<std::is_move_constructible_v<U>>>
    explicit CacheLineAligned(T&& v)
        noexcept(std::is_nothrow_move_constructible_v<T>)
        : value(static_cast<T&&>(v)) {}

    operator T&()              noexcept { return value; }
    operator const T&() const  noexcept { return value; }

    CacheLineAligned& operator=(const T& v)
        noexcept(std::is_nothrow_copy_assignable_v<T>) { value = v; return *this; }
    CacheLineAligned& operator=(T&& v)
        noexcept(std::is_nothrow_move_assignable_v<T>) { value = static_cast<T&&>(v); return *this; }
};

}  // namespace core
}  // namespace axob

// -----------------------------------------------------------------
//  CACHELINE_PAD(n)
//  在结构体中产生 n 字节的 padding，使后续成员跳过 n 字节。
//  每次展开生成唯一变量名（利用 __LINE__）。
//
//  用法示例：
//    struct Foo {
//        std::atomic<uint64_t> counter;
//        CACHELINE_PAD(56);                // 填满剩余 cache line
//        std::atomic<uint64_t> other;
//    };
// -----------------------------------------------------------------
#define AXOB_CACHELINE_PAD_CONCAT_INNER(a, b) a##b
#define AXOB_CACHELINE_PAD_CONCAT(a, b) AXOB_CACHELINE_PAD_CONCAT_INNER(a, b)
#define CACHELINE_PAD(n) uint8_t AXOB_CACHELINE_PAD_CONCAT(_cacheline_pad_, __LINE__)[(n)]
