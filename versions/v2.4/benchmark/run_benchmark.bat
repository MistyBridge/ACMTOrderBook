@echo off
REM =====================================================================
REM  run_benchmark.bat — 哈希表基准测试一键运行脚本
REM
REM  用法：
REM    run_benchmark.bat              # 仅测试 std::unordered_map
REM    run_benchmark.bat robin_hood   # 测试 robin_hood
REM    run_benchmark.bat phmap        # 测试 phmap::flat_hash_map
REM    run_benchmark.bat all          # 测试所有哈希表
REM =====================================================================

setlocal enabledelayedexpansion

set BUILD_DIR=build_benchmark
set BENCHMARK_EXE=%BUILD_DIR%\Release\hashmap_benchmark.exe

echo.
echo ============================================================
echo  ACMTOrderBook v2.2 — 哈希表性能基准测试
echo ============================================================
echo.

REM 解析参数
set TEST_TARGET=%1
if "%TEST_TARGET%"=="" set TEST_TARGET=std

REM 构建选项
set CMAKE_OPTIONS=-DBUILD_BENCHMARK=ON

if "%TEST_TARGET%"=="robin_hood" (
    echo [INFO] 启用 robin_hood::unordered_map
    set CMAKE_OPTIONS=%CMAKE_OPTIONS% -DUSE_ROBIN_HOOD=ON
) else if "%TEST_TARGET%"=="phmap" (
    echo [INFO] 启用 phmap::flat_hash_map
    set CMAKE_OPTIONS=%CMAKE_OPTIONS% -DUSE_PHMAP=ON
) else if "%TEST_TARGET%"=="all" (
    echo [INFO] 启用所有哈希表
    set CMAKE_OPTIONS=%CMAKE_OPTIONS% -DUSE_ROBIN_HOOD=ON -DUSE_PHMAP=ON -DUSE_UNORDERED_DENSE=ON
) else if "%TEST_TARGET%"=="std" (
    echo [INFO] 仅测试 std::unordered_map
) else (
    echo [ERROR] 未知参数: %TEST_TARGET%
    echo 用法: run_benchmark.bat [std^|robin_hood^|phmap^|all]
    exit /b 1
)

echo.
echo [步骤 1/3] 生成构建系统...
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
cmake -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 %CMAKE_OPTIONS%
if errorlevel 1 (
    echo [ERROR] CMake 配置失败
    exit /b 1
)

echo.
echo [步骤 2/3] 编译基准测试...
cmake --build %BUILD_DIR% --config Release --target hashmap_benchmark --parallel 8
if errorlevel 1 (
    echo [ERROR] 编译失败
    exit /b 1
)

echo.
echo [步骤 3/3] 运行基准测试...
echo.
%BENCHMARK_EXE%
if errorlevel 1 (
    echo [ERROR] 基准测试运行失败
    exit /b 1
)

echo.
echo ============================================================
echo  基准测试完成
echo ============================================================
echo.

endlocal