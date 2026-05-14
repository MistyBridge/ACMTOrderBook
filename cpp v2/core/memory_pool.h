#pragma once
// =====================================================================
//  core/memory_pool.h — 对象内存池
//
//  预分配连续内存块，划分为 sizeof(T) 大小的 slot。
//  使用侵入式空闲链表（intrusive free list）：
//    每个空闲 slot 的头 sizeof(void*) 字节存储指向下一个空闲 slot 的指针。
//  扩容策略：当空闲链表为空时以 2 倍增长。
//
//  线程安全：**不需要**！本池仅被单个消费者线程访问。
//
//  模板参数 T 必须满足 sizeof(T) >= sizeof(void*)，以便在空闲 slot 中
//  存储链表指针。
//
//  用法示例：
//    MemoryPool<ObOrder> pool(4096);
//    ObOrder* order = pool.alloc();
//    // ... 使用 order ...
//    pool.free(order);
// =====================================================================

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

namespace axob {
namespace core {

template <typename T>
class MemoryPool {
    static_assert(sizeof(T) >= sizeof(void*),
                  "MemoryPool<T>: sizeof(T) must be >= sizeof(void*) for intrusive free list");
    static_assert(alignof(T) >= 1,
                  "MemoryPool<T>: T must be a complete type");

public:
    // ---------------------------------------------------------------
    //  构造函数
    //  initialCount: 预分配 slot 数量
    // ---------------------------------------------------------------
    explicit MemoryPool(size_t initialCount = 1024)
        : slotSize_(calcSlotSize())
        , alignment_(calcAlignment())
    {
        if (initialCount == 0) initialCount = 1;
        allocateBlock(initialCount);
    }

    ~MemoryPool() {
        // 释放所有已分配的内存块
        for (auto& blk : blocks_) {
            alignedFree(blk.memory);
        }
    }

    // 禁止拷贝/移动
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&)                 = delete;
    MemoryPool& operator=(MemoryPool&&)      = delete;

    // ---------------------------------------------------------------
    //  alloc() —— 从空闲链表取出一个 slot 并返回其指针
    //  空闲链表为空时自动扩容（2 倍增长）
    // ---------------------------------------------------------------
    T* alloc() {
        if (freeList_ == nullptr) {
            // 扩容：分配当前总量 2 倍的 slot
            size_t growCount = (totalSlots_ > 0) ? totalSlots_ : 1;
            allocateBlock(growCount);
        }

        // 从链表头摘下一个 slot
        T* slot = freeList_;
        FreeNode* node = reinterpret_cast<FreeNode*>(slot);
        freeList_ = node->next;
        --freeCount_;
        return slot;
    }

    // ---------------------------------------------------------------
    //  free(T*) —— 归还 slot 到空闲链表
    // ---------------------------------------------------------------
    void free(T* p) {
        if (p == nullptr) return;
        FreeNode* node = reinterpret_cast<FreeNode*>(p);
        node->next = freeList_;
        freeList_ = p;
        ++freeCount_;
    }

    // ---------------------------------------------------------------
    //  totalSlots() —— 已分配的总 slot 数量
    // ---------------------------------------------------------------
    size_t totalSlots() const noexcept { return totalSlots_; }

    // ---------------------------------------------------------------
    //  freeSlots() —— 当前空闲 slot 数量
    // ---------------------------------------------------------------
    size_t freeSlots() const noexcept { return freeCount_; }

private:
    // 空闲链表节点（存储在空闲 slot 的头部）
    struct FreeNode {
        T* next;
    };

    // 计算 slot 实际大小：至少 sizeof(T) 和 sizeof(FreeNode)，并对齐到 alignment
    static size_t calcSlotSize() noexcept {
        size_t sz = sizeof(T);
        // 确保 slot 能容纳 FreeNode（虽然 static_assert 已保证，双重保险）
        if (sz < sizeof(FreeNode)) sz = sizeof(FreeNode);
        return sz;
    }

    // 对齐值：取 alignof(T) 与 alignof(std::max_align_t) 的较大者
    static size_t calcAlignment() noexcept {
        size_t a = alignof(T);
        if (a < alignof(std::max_align_t)) a = alignof(std::max_align_t);
        return a;
    }

    // 分配 count 个 slot 的新内存块并将其全部链入空闲链表
    void allocateBlock(size_t count) {
        size_t blockBytes = count * slotSize_;
        // 向上取整到 alignment 的倍数（aligned_alloc 要求）
        blockBytes = (blockBytes + alignment_ - 1) & ~(alignment_ - 1);

        void* mem = alignedAlloc(blockBytes, alignment_);
        if (!mem) std::abort();
        std::memset(mem, 0, blockBytes);  // 归零，确保指针字段初始为空

        blocks_.push_back({mem, blockBytes});

        // 将新 block 中的所有 slot 串成链表，挂到 freeList_ 前端
        char* ptr = static_cast<char*>(mem);
        for (size_t i = 0; i < count; ++i) {
            FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
            if (i + 1 < count) {
                node->next = reinterpret_cast<T*>(ptr + slotSize_);
            } else {
                // 最后一个 slot 指向原来的 freeList_（可能为 nullptr）
                node->next = freeList_;
            }
            ptr += slotSize_;
        }
        freeList_  = reinterpret_cast<T*>(mem);
        totalSlots_ += count;
        freeCount_  += count;
    }

    // ---- 平台相关的对齐分配/释放 ----
    static void* alignedAlloc(size_t size, size_t alignment) {
#ifdef _WIN32
        return _aligned_malloc(size, alignment);
#else
        // std::aligned_alloc 要求 size 是 alignment 的倍数（已在调用方保证）
        return std::aligned_alloc(alignment, size);
#endif
    }

    static void alignedFree(void* p) {
#ifdef _WIN32
        _aligned_free(p);
#else
        std::free(p);
#endif
    }

    // ---- 数据成员 ----
    const size_t slotSize_;     // 每个 slot 的字节数
    const size_t alignment_;    // 内存对齐值

    T*      freeList_  = nullptr;   // 空闲链表头
    size_t  totalSlots_ = 0;        // 已分配 slot 总数
    size_t  freeCount_  = 0;        // 当前空闲 slot 数

    // 已分配的内存块列表（析构时释放）
    struct Block {
        void*  memory;
        size_t size;
    };
    std::vector<Block> blocks_;
};

}  // namespace core
}  // namespace axob