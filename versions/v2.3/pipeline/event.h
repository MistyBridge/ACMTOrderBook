#pragma once
#include <cstdint>
#include <chrono>
#include "../tool/axsbe_base.h"
#include "../tool/axsbe_order.h"
#include "../tool/axsbe_exe.h"
#include "../tool/axsbe_snap_stock.h"
#include "../behave/ob_types.h"  // AXSignal

// =====================================================================
//  MarketEvent — 统一消息事件
//  生产者-消费者管道中的传输单元
// =====================================================================

enum class EventType : uint8_t {
    ORDER   = 0,
    EXEC    = 1,
    SNAP    = 2,
    SIGNAL  = 3,
    END     = 4,  // 生产者已读完文件，通知消费者退出
};

inline uint64_t now_ns() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

struct MarketEvent {
    EventType type = EventType::END;
    uint64_t enqueueTimestamp = 0;  // 生产者写入队列时的时间戳（纳秒）

    // Union 持有不同类型的消息
    union {
        AxsbeOrder     order;
        AxsbeExe       exec;
        AxsbeSnapStock snap;
        AXSignal       signal;
    };

    // 默认构造函数（SPSC队列需要）
    MarketEvent() : type(EventType::END), enqueueTimestamp(0), signal(AXSignal::ALL_END) {}

    // 析构函数（union 中有非 trivial 类型，但此处 Axsbe* 都是 POD-like）
    ~MarketEvent() {}

    // 拷贝构造和赋值（SPSC队列需要）
    MarketEvent(const MarketEvent& other) : type(other.type), enqueueTimestamp(other.enqueueTimestamp) {
        switch (type) {
            case EventType::ORDER:  new (&order) AxsbeOrder(other.order); break;
            case EventType::EXEC:   new (&exec) AxsbeExe(other.exec); break;
            case EventType::SNAP:   new (&snap) AxsbeSnapStock(other.snap); break;
            case EventType::SIGNAL: signal = other.signal; break;
            case EventType::END:    signal = other.signal; break;
        }
    }

    MarketEvent& operator=(const MarketEvent& other) {
        if (this != &other) {
            type = other.type;
            enqueueTimestamp = other.enqueueTimestamp;
            switch (type) {
                case EventType::ORDER:  new (&order) AxsbeOrder(other.order); break;
                case EventType::EXEC:   new (&exec) AxsbeExe(other.exec); break;
                case EventType::SNAP:   new (&snap) AxsbeSnapStock(other.snap); break;
                case EventType::SIGNAL: signal = other.signal; break;
                case EventType::END:    signal = other.signal; break;
            }
        }
        return *this;
    }

    // ---- 便捷静态工厂方法 ----
    static MarketEvent makeOrder(const AxsbeOrder& o) {
        MarketEvent ev;
        ev.type = EventType::ORDER;
        ev.enqueueTimestamp = now_ns();
        new (&ev.order) AxsbeOrder(o);
        return ev;
    }

    static MarketEvent makeExe(const AxsbeExe& e) {
        MarketEvent ev;
        ev.type = EventType::EXEC;
        ev.enqueueTimestamp = now_ns();
        new (&ev.exec) AxsbeExe(e);
        return ev;
    }

    static MarketEvent makeSnap(const AxsbeSnapStock& s) {
        MarketEvent ev;
        ev.type = EventType::SNAP;
        ev.enqueueTimestamp = now_ns();
        new (&ev.snap) AxsbeSnapStock(s);
        return ev;
    }

    static MarketEvent makeSignal(AXSignal sig) {
        MarketEvent ev;
        ev.type = EventType::SIGNAL;
        ev.enqueueTimestamp = now_ns();
        ev.signal = sig;
        return ev;
    }

    static MarketEvent makeEnd() {
        MarketEvent ev;
        ev.type = EventType::END;
        ev.enqueueTimestamp = now_ns();
        ev.signal = AXSignal::ALL_END;
        return ev;
    }
};
