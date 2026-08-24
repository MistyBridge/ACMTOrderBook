#pragma once
// [v2.5] Large page support — sandbox fallback.
//
// The original huge_pages.h is platform-specific and was not committed with the
// historical source. Large pages are not available in this MinGW sandbox, so
// provide the same contract: allocLargePages returns nullptr (MemoryPool then
// falls back to alignedAlloc) and freeLargePages is a no-op. This does not
// change engine semantics; it simply disables the large-page optimization.
#include <cstddef>

static inline void* allocLargePages(std::size_t) { return nullptr; }
static inline void freeLargePages(void*, std::size_t) {}
