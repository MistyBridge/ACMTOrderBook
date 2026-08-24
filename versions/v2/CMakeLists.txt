cmake_minimum_required(VERSION 3.14)
project(AXOrderBook_v2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    target_compile_definitions(orderbook_v2 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# 编译选项
target_compile_options(orderbook_v2 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /O2 /arch:AVX2>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -O2 -march=native -DNDEBUG>
)

# msg_util.cpp 需要 -fno-inline 以规避 MinGW 8.1.0 -O2 内联 bug
set_source_files_properties(tool/msg_util.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-inline>"
)
