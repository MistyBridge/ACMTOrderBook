cmake_minimum_required(VERSION 3.14)
project(AXOrderBook_v2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 并行编译（Ninja 自动并行；Makefile 需要 cmake --build build --parallel 8）
set(CMAKE_BUILD_PARALLEL_LEVEL 8)

# Windows 下使用静态运行时库
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR})

# 源文件列表
set(SOURCES
    main.cpp
    pipeline/pipeline.cpp
    pipeline/producer.cpp
    pipeline/consumer.cpp
    tool/msg_util.cpp
    behave/axob_init.cpp
    behave/axob_order.cpp
    behave/axob_trade.cpp
    behave/axob_cage.cpp
    behave/axob_snap.cpp
)

# 创建可执行文件
add_executable(orderbook_v2 ${SOURCES})

# 链接线程库
find_package(Threads REQUIRED)
target_link_libraries(orderbook_v2 PRIVATE Threads::Threads)

# Windows 特定设置
if(WIN32)
    target_compile_definitions(orderbook_v2 PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# 编译选项
target_compile_options(orderbook_v2 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4
        /O2
        /arch:AVX2
        /GL               # 全程序优化
    >
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
        -Wall -Wextra
        -O3                         # 最高优化级别
        -march=native               # 利用本机 CPU 指令集
        -funroll-loops              # 循环展开
        -fomit-frame-pointer        # 省略帧指针，多一个寄存器可用
        -flto                       # 链接时优化（编译端）
        -DNDEBUG                    # 禁用 assert
    >
)

# 链接选项
target_link_options(orderbook_v2 PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
        -flto                       # 链接时优化（链接端）
        -fuse-linker-plugin         # LTO 插件
    >
)

if(MSVC)
    target_link_options(orderbook_v2 PRIVATE /LTCG)  # 链接时代码生成
endif()

# msg_util.cpp 需要 -fno-inline 以规避 MinGW 8.1.0 内联 bug（-O2/-O3 下
# parseKeyValueLine 含 try/catch+std::stoll 被内联后栈损坏）
# LTO 模式下 -fno-inline 会嵌入 LTO bytecode，链接器插件自动尊重该属性
set_source_files_properties(tool/msg_util.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-inline>"
)
