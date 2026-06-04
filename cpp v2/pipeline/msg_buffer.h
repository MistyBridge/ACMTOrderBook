#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include "../tool/axsbe_order.h"
#include "../tool/axsbe_exe.h"
#include "../tool/axsbe_snap_stock.h"
#include "../behave/ob_types.h"

// 缓存行大小（与 cache_line.h 保持一致）
static constexpr size_t MSG_CACHELINE_SIZE = 64;

// =====================================================================
//  MsgSlot — 环形缓冲区中的消息槽位
//
//  每个槽位包含：消息类型 + 消息数据 + 同步标志
//  使用 ready/consumed 标志实现无锁 SPSC 同步
// =====================================================================

enum class SlotType : uint8_t {
    EMPTY  = 0,
    ORDER  = 1,
    EXEC   = 2,
    SNAP   = 3,
    SIGNAL = 4,
    END    = 5,
};

struct MsgSlot {
    SlotType type = SlotType::EMPTY;
    union {
        AxsbeOrder     order;
        AxsbeExe       exec;
        AxsbeSnapStock snap;
        AXSignal       signal;
    };

    MsgSlot() : type(SlotType::EMPTY), signal(AXSignal::ALL_END) {}
};

// =====================================================================
//  MsgRingBuffer — SPSC 环形消息缓冲区
//
//  生产者写入消息数据到槽位，消费者直接从槽位读取
//  通过队列传递 4 字节索引而非 500 字节消息
//
//  同步模型：
//    生产者：wait consumed==true → write data → ready=true(release) → push index
//    消费者：pop index → wait ready==true(acquire) → read data → consumed=true(release)
// =====================================================================

static constexpr size_t MSG_RING_SIZE = 64;  // 必须是 2 的幂
static constexpr size_t MSG_RING_MASK = MSG_RING_SIZE - 1;

struct alignas(MSG_CACHELINE_SIZE) MsgRingBuffer {
    struct Slot {
        MsgSlot msg;
        std::atomic<bool> ready{false};
        std::atomic<bool> consumed{true};  // 初始为 true（可写入）
    };

    Slot slots[MSG_RING_SIZE];

    // 获取槽位指针（供队列传递索引用）
    Slot* getSlot(size_t idx) { return &slots[idx & MSG_RING_MASK]; }
    const Slot* getSlot(size_t idx) const { return &slots[idx & MSG_RING_MASK]; }
};
