cmake_minimum_required(VERSION 3.14)
project(AXOrderBook_v2 LANGUAGES CXX)

# =====================================================================
#  AXOrderBook v2 — 多线程订单簿撮合引擎  CMakeLists.txt
#
# =====================================================================
#  一、环境要求
# =====================================================================
#
#  1. Visual Studio 2022 Build Tools (v143 工具集)
#     安装时勾选："使用 C++ 的桌面开发" 工作负载
#     包含组件：MSVC v143、Windows SDK、CMake 组件
#
#  2. Windows SDK ≥ 10.0.26100.0
#
#  3. cmake ≥ 3.14（VS 2022 Build Tools 自带 3.31）
#
# =====================================================================
#  二、完整构建流程（在项目根目录执行）
# =====================================================================
#
#  步骤 1 — 生成构建系统（仅首次，或 CMakeLists.txt 修改后）
#    cmake -B build -G "Visual Studio 17 2022" -A x64
#
#  步骤 2 — 编译 Release 版本
#    cmake --build build --config Release --parallel 8
#
#  步骤 3 — 复制产物并重命名
#    copy build\Release\orderbook_v2.exe orderbook_v2.1.exe
#
#  产出文件：项目根目录/orderbook_v2.1.exe
#
# =====================================================================
#  三、MSVC 实际生效的编译选项一览（Release|x64）
# =====================================================================
#
#  【编译器 cl.exe】
#    /c                                    编译但不链接
#    /I"<项目根目录>"                       头文件搜索路径
#    /nologo                               抑制版权横幅
#    /W4                                   警告级别 4（接近最高）
#    /WX-                                  警告不作为错误
#    /diagnostics:column                   诊断信息显示列号
#    /O2                                   最大速度优化（相当于 /Ob2 /Oi /Ot /Oy）
#    /Ob2                                  内联策略：任何合适的函数
#    /GL                                   全程序优化（配合链接器 /LTCG）
#    /D _MBCS                              多字节字符集
#    /D WIN32                              Win32 平台宏
#    /D _WINDOWS                           Windows 应用程序
#    /D NDEBUG                             禁用 assert()
#    /D _CRT_SECURE_NO_WARNINGS            禁用 sprintf/strcpy 等安全警告
#    /D WIN32_LEAN_AND_MEAN                排除不常用 Windows 头文件，加速编译
#    /D NOMINMAX                           阻止 windows.h 定义 min/max 宏
#    /EHsc                                 同步异常处理（C++ 标准）
#    /MD                                   动态链接运行时库（MSVCRT）
#    /GS                                   缓冲区安全检查
#    /arch:AVX2                            启用 AVX2 指令集
#    /fp:precise                           浮点模型：精确（默认）
#    /Zc:wchar_t                           wchar_t 作为内置类型
#    /Zc:forScope                          for 循环变量作用域遵循标准
#    /Zc:inline                            内联链接规范遵循标准
#    /GR                                   启用运行时类型信息 (RTTI)
#    /std:c++17                            C++17 语言标准
#    /utf-8                                源文件视为 UTF-8 编码
#                                          （源码含中文注释，不加此选项 MSVC 按
#                                          GBK(code page 936) 解析，导致 C4819
#                                          警告及 cache_line.h 宏展开错乱 C1004）
#    /Gd                                   默认调用约定 __cdecl
#    /TP                                   强制将所有源文件视为 C++
#    /external:W4                          外部头文件警告级别
#
#  【链接器 link.exe】
#    /OUT:"build\Release\orderbook_v2.exe" 输出文件
#    /INCREMENTAL:NO                       禁用增量链接（Release 优化）
#    /NOLOGO                               抑制版权横幅
#    /MANIFEST                             生成清单
#    /MANIFESTUAC:"level='asInvoker'"      UAC 级别：以调用者身份运行
#    /manifest:embed                       清单嵌入到可执行文件
#    /PDB:..."orderbook_v2.pdb"            程序数据库（调试符号，Release 下不生成调试信息）
#    /SUBSYSTEM:CONSOLE                    控制台子系统
#    /LTCG                                 链接时代码生成（配合编译器 /GL）
#    /DYNAMICBASE                          地址空间布局随机化 (ASLR)
#    /NXCOMPAT                             数据执行保护 (DEP)
#    /MACHINE:X64                          目标平台 x64
#    链接库：kernel32 user32 gdi32 winspool shell32
#            ole32 oleaut32 uuid comdlg32 advapi32
#
# =====================================================================
#  四、GCC/Clang 编译注意事项（源码已做跨平台适配）
# =====================================================================
#
#  1. GCC 内联汇编 __asm__ 不兼容 MSVC
#     → 已在 pipeline/producer.cpp 中封装 cpu_pause()，MSVC 用 _mm_pause()
#
#  2. __attribute__((noinline)) 不兼容 MSVC
#     → 已用 #ifdef __GNUC__ 守卫（见 tool/axsbe_order.h 等）
#
#  3. __builtin_expect / __builtin_prefetch 不兼容 MSVC
#     → 已用条件宏回退（见 behave/ob_types.h LIKELY/UNLIKELY
#       和 pipeline/consumer.cpp PREFETCH_READ）
#
# =====================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 并行编译（Ninja 自动并行；Makefile 需要 cmake --build build --parallel 8）
set(CMAKE_BUILD_PARALLEL_LEVEL 8)

# 头文件目录（项目根目录，使 #include "core/xxx.h" 等相对路径可用）
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

# -----------------------------------------------------------------
#  MSVC 运行时库：/MD（动态链接 MSVCRT.DLL）
#
#  实际 vcxproj 生成结果：
#    Release → MultiThreadedDLL     (/MD)
#    Debug   → MultiThreadedDebugDLL (/MDd)
#
#  说明：CMake Visual Studio 生成器在 3.x 版本中会将
#  CMAKE_MSVC_RUNTIME_LIBRARY 全局变量与 VS 默认值合并，
#  最终 vcxproj 中的 RuntimeLibrary 取决于生成器内部逻辑，
#  可能与变量设置不一致。此处不做 set_property 覆盖，
#  以保持与 vcxproj 实际生成内容一致。
#  如需切换为静态链接 /MT，请在 vcxproj 中手动修改：
#    MultiThreadedDLL → MultiThreaded
#    MultiThreadedDebugDLL → MultiThreadedDebug
# -----------------------------------------------------------------

# 链接线程库（MSVC 下 Threads::FOUND=TRUE 但实际链接库列表为空，
# Windows 原生支持线程，无需额外链接）
find_package(Threads REQUIRED)
target_link_libraries(orderbook_v2 PRIVATE Threads::Threads)

# Windows 特定预处理器定义
if(WIN32)
    target_compile_definitions(orderbook_v2 PRIVATE
        _CRT_SECURE_NO_WARNINGS    # 禁用 sprintf/strcpy 等安全函数警告
        WIN32_LEAN_AND_MEAN        # 排除不常用的 Windows 头文件，加速编译
        NOMINMAX                   # 阻止 windows.h 定义 min/max 宏，避免与 <algorithm> 冲突
    )
endif()

# 编译选项
target_compile_options(orderbook_v2 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:
        /utf-8             # 将源文件视为 UTF-8 编码
                           # 源码含中文注释，不加此选项 MSVC 按 GBK(code page 936) 解析
                           # 导致 C4819 警告及 cache_line.h 宏展开错乱 (C1004)
        /W4                # 警告级别 4
        /O2                # 最大速度优化（Release）
        /arch:AVX2         # 启用 AVX2 指令集
        /GL                # 全程序优化（配合链接器 /LTCG）
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
    target_link_options(orderbook_v2 PRIVATE /LTCG)  # 链接时代码生成（配合 /GL）
endif()

# msg_util.cpp 需要 -fno-inline 以规避 MinGW 8.1.0 内联 bug（-O2/-O3 下
# parseKeyValueLine 含 try/catch+std::stoll 被内联后栈损坏）
# LTO 模式下 -fno-inline 会嵌入 LTO bytecode，链接器插件自动尊重该属性
set_source_files_properties(tool/msg_util.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-inline>"
)
