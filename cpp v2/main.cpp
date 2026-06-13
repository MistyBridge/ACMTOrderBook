#include <cstdio>
#include <cstdlib>
#include "pipeline/pipeline.h"

int main(int argc, char* argv[]) {
    // 命令行参数解析
    const char* dataFile    = (argc > 1) ? argv[1] : "../data/20220422/AX_sbe_szse_000001.log";
    int producerCore        = (argc > 2) ? atoi(argv[2]) : 0;
    int consumerCore        = (argc > 3) ? atoi(argv[3]) : 2;
    size_t queueCapacity    = (argc > 4) ? (size_t)atol(argv[4]) : 16384;
    size_t batchSize        = (argc > 5) ? (size_t)atol(argv[5]) : 64;

    // 打印启动信息
    printf("[v2] Multi-threaded OrderBook Engine\n");
    printf("[v2] Queue capacity: %zu, Batch size: %zu\n", queueCapacity, batchSize);
    printf("[v2] Producer -> Core %d, Consumer -> Core %d\n", producerCore, consumerCore);
    fflush(stdout);

    // 创建并运行 Pipeline
    Pipeline pipeline(dataFile, queueCapacity, batchSize, producerCore, consumerCore);
    pipeline.run();

    return 0;
}
