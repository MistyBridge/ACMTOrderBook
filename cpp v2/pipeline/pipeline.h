#pragma once
#include <string>
#include "../core/latency_stats.h"
#include "../core/spsc_queue.h"
#include "event.h"
#include "producer.h"
#include "consumer.h"

// =====================================================================
//  Pipeline — 管道启动器
//  管理生产者-消费者线程的生命周期
// =====================================================================
class Pipeline {
public:
    // 结果结构体
    struct Result {
        int totalMsgs = 0;
        int orderCnt  = 0;
        int exeCnt    = 0;
        int snapCnt   = 0;
        double elapsedSec  = 0.0;
        double throughput   = 0.0;
        axob::core::LatencyStats::Stats latency{};
        uint64_t producedTimeNs = 0;
        uint64_t consumedTimeNs = 0;
    };

    // 构造函数
    Pipeline(const char* dataFile,
             size_t queueCapacity = 16384,
             size_t batchSize = 64,
             int producerCore = 0,
             int consumerCore = 2,
             int replayCount = 1);  // 重放次数

    // 启动管道，阻塞直到处理完毕
    void run();

    // 获取结果（run() 结束后调用）
    Result getResult() const;

private:
    const char* dataFile_;
    size_t queueCapacity_;
    size_t batchSize_;
    int producerCore_;
    int consumerCore_;
    int replayCount_;  // 重放次数
    Result result_;
};
