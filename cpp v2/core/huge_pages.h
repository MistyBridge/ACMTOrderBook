#pragma once
// =====================================================================
//  core/huge_pages.h - Large Page (Huge Page) memory allocation
//
//  Cross-platform wrapper for allocating memory with large pages.
//  Large pages (2MB on x86-64) reduce TLB misses for large allocations.
//
//  Windows: Requires "Lock pages in memory" privilege (secpol.msc)
//  Linux: Requires hugetlbfs or transparent huge pages
//
//  Usage:
//    void* p = axob::core::allocLargePages(2 * 1024 * 1024);
//    if (p) { /* use large pages */ }
//    axob::core::freeLargePages(p, 2 * 1024 * 1024);
// =====================================================================

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
#endif

namespace axob {
namespace core {

// -----------------------------------------------------------------
//  hasLargePages() - Check if large pages are available
// -----------------------------------------------------------------
inline bool hasLargePages() {
#ifdef _WIN32
    return GetLargePageMinimum() > 0;
#else
    // Try a small mmap with HUGETLB to test
    void* test = mmap(nullptr, 2 * 1024 * 1024, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (test == MAP_FAILED) return false;
    munmap(test, 2 * 1024 * 1024);
    return true;
#endif
}

// -----------------------------------------------------------------
//  largePageSize() - Get the large page size (typically 2MB)
// -----------------------------------------------------------------
inline size_t largePageSize() {
#ifdef _WIN32
    size_t sz = GetLargePageMinimum();
    return sz > 0 ? sz : 4096;
#else
    long sz = sysconf(_SC_PAGESIZE);
    // On x86-64, huge pages are typically 2MB
    return 2 * 1024 * 1024;  // Assume 2MB huge pages
#endif
}

// -----------------------------------------------------------------
//  allocLargePages() - Allocate memory using large pages
//  Returns nullptr if large pages are not available.
//  Size is rounded up to large page boundary.
// -----------------------------------------------------------------
inline void* allocLargePages(size_t size) {
#ifdef _WIN32
    size_t lpSize = GetLargePageMinimum();
    if (lpSize == 0) return nullptr;

    // Round up to large page boundary
    size = (size + lpSize - 1) & ~(lpSize - 1);

    return VirtualAlloc(
        nullptr, size,
        MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
        PAGE_READWRITE);
#else
    // Round up to 2MB boundary
    const size_t HP_SIZE = 2 * 1024 * 1024;
    size = (size + HP_SIZE - 1) & ~(HP_SIZE - 1);

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    return (ptr == MAP_FAILED) ? nullptr : ptr;
#endif
}

// -----------------------------------------------------------------
//  freeLargePages() - Free large page allocation
// -----------------------------------------------------------------
inline void freeLargePages(void* ptr, size_t size) {
    if (!ptr) return;
#ifdef _WIN32
    (void)size;  // VirtualFree doesn't need size
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

// -----------------------------------------------------------------
//  allocWithFallback() - Try large pages, fall back to aligned alloc
//  Always returns a valid pointer (aborts on failure).
// -----------------------------------------------------------------
inline void* allocWithFallback(size_t size, size_t alignment = 64) {
    // Try large pages first
    void* ptr = allocLargePages(size);
    if (ptr) return ptr;

    // Fallback to aligned allocation
#ifdef _WIN32
    ptr = _aligned_malloc(size, alignment);
#else
    ptr = std::aligned_alloc(alignment, size);
#endif
    if (!ptr) std::abort();
    return ptr;
}

// -----------------------------------------------------------------
//  freeWithFallback() - Free memory allocated by allocWithFallback
//  Note: We cannot easily distinguish large page vs aligned alloc.
//  For simplicity, use VirtualFree/free on all pointers.
//  On Windows, VirtualFree works for both VirtualAlloc and _aligned_malloc
//  if we track the source. For now, callers must know which they used.
// -----------------------------------------------------------------

}  // namespace core
}  // namespace axob
