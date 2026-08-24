// test_mmap.cpp — mmap 文件读取器测试
// 编译：g++ -std=c++17 -O3 -o test_mmap test_mmap.cpp tool/msg_util.cpp -I.

#include "tool/mmap_file.h"
#include "tool/msg_util.h"
#include <iostream>
#include <chrono>

int main() {
    const char* testFile = "../data/20220422/AX_sbe_szse_000001.log";

    std::cout << "=== mmap 文件读取器测试 ===" << std::endl;
    std::cout << "测试文件: " << testFile << std::endl;

    // 测试 1: MmapFile 基础功能
    std::cout << "\n[测试 1] MmapFile 基础功能" << std::endl;
    try {
        MmapFile mmapFile(testFile);
        std::cout << "  文件大小: " << mmapFile.size() << " bytes" << std::endl;
        std::cout << "  数据有效: " << (mmapFile.isValid() ? "是" : "否") << std::endl;
        std::cout << "  前 100 字节: ";
        for (size_t i = 0; i < std::min<size_t>(100, mmapFile.size()); ++i) {
            std::cout << mmapFile.data()[i];
        }
        std::cout << "..." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "  错误: " << e.what() << std::endl;
        return 1;
    }

    // 测试 2: MmapFileReader 消息读取
    std::cout << "\n[测试 2] MmapFileReader 消息读取" << std::endl;
    try {
        MmapFileReader reader(testFile);

        int orderCount = 0;
        int exeCount = 0;
        int snapCount = 0;
        int totalCount = 0;

        auto start = std::chrono::high_resolution_clock::now();

        while (reader.hasNext()) {
            AxsbeOrder order;
            AxsbeExe exe;
            AxsbeSnapStock snap;

            int type = reader.next(order, exe, snap);
            totalCount++;

            if (type == MsgType_order) {
                orderCount++;
            } else if (type == MsgType_exe) {
                exeCount++;
            } else if (type == MsgType_snap) {
                snapCount++;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "  总消息数: " << totalCount << std::endl;
        std::cout << "  委托消息: " << orderCount << std::endl;
        std::cout << "  成交消息: " << exeCount << std::endl;
        std::cout << "  快照消息: " << snapCount << std::endl;
        std::cout << "  读取耗时: " << duration.count() << " ms" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  错误: " << e.what() << std::endl;
        return 1;
    }

    // 测试 3: AxsbeFileReader 对比
    std::cout << "\n[测试 3] AxsbeFileReader 对比" << std::endl;
    try {
        AxsbeFileReader reader(testFile);

        int orderCount = 0;
        int exeCount = 0;
        int snapCount = 0;
        int totalCount = 0;

        auto start = std::chrono::high_resolution_clock::now();

        while (reader.hasNext()) {
            AxsbeOrder order;
            AxsbeExe exe;
            AxsbeSnapStock snap;

            int type = reader.next(order, exe, snap);
            totalCount++;

            if (type == MsgType_order) {
                orderCount++;
            } else if (type == MsgType_exe) {
                exeCount++;
            } else if (type == MsgType_snap) {
                snapCount++;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "  总消息数: " << totalCount << std::endl;
        std::cout << "  委托消息: " << orderCount << std::endl;
        std::cout << "  成交消息: " << exeCount << std::endl;
        std::cout << "  快照消息: " << snapCount << std::endl;
        std::cout << "  读取耗时: " << duration.count() << " ms" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  错误: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n=== 测试完成 ===" << std::endl;

    return 0;
}