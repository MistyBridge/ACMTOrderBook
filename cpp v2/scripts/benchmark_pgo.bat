@echo off
REM =====================================================================
REM  benchmark_pgo.bat — PGO 性能对比测试
REM
REM  用法：
REM    benchmark_pgo.bat           # 运行性能对比测试
REM    benchmark_pgo.bat baseline  # 仅运行基线测试
REM    benchmark_pgo.bat pgo       # 仅运行 PGO 测试
REM
REM  输出：
REM    - 基线版本吞吐量
REM    - PGO 版本吞吐量
REM    - 性能提升百分比
REM =====================================================================

setlocal enabledelayedexpansion

set BASELINE_EXE=build\Release\orderbook_v2.exe
set PGO_EXE=build_pgo\Release\orderbook_v2.exe
set DATA_FILE=../data/20220422/AX_sbe_szse_000001.log
set ITERATIONS=10

echo.
echo ============================================================
echo  ACMTOrderBook v2.5 — PGO 性能对比测试
echo ============================================================
echo.

REM 解析参数
set ACTION=%1
if "%ACTION%"=="" set ACTION=all

REM 检查可执行文件
if not exist %BASELINE_EXE% (
    echo [ERROR] 基线版本不存在: %BASELINE_EXE%
    echo 请先编译普通版本
    exit /b 1
)

if "%ACTION%"=="baseline" (
    goto :test_baseline
) else if "%ACTION%"=="pgo" (
    goto :test_pgo
) else if "%ACTION%"=="all" (
    goto :test_all
) else (
    echo [ERROR] 未知参数: %ACTION%
    echo 用法: benchmark_pgo.bat [baseline^|pgo^|all]
    exit /b 1
)

REM =====================================================================
REM  基线测试
REM =====================================================================
:test_baseline
echo [测试 1/2] 基线版本性能测试
echo   运行 %ITERATIONS% 次重放...
echo.

set BASELINE_TOTAL=0
set BASELINE_COUNT=0

for /l %%i in (1,1,%ITERATIONS%) do (
    echo   [%%i/%ITERATIONS%] 运行中...
    %BASELINE_EXE% %DATA_FILE% 0 2 16384 64 1 > temp_output.txt 2>&1

    REM 解析输出获取吞吐量
    for /f "tokens=*" %%a in ('findstr "msg/s" temp_output.txt') do (
        set LINE=%%a
        REM 提取数字部分
        for /f "tokens=2" %%b in ("!LINE!") do (
            set THROUGHPUT=%%b
            set /a BASELINE_TOTAL+=!THROUGHPUT!
            set /a BASELINE_COUNT+=1
        )
    )
)

if %BASELINE_COUNT% gtr 0 (
    set /a BASELINE_AVG=%BASELINE_TOTAL% / %BASELINE_COUNT%
    echo   基线版本平均吞吐量: %BASELINE_AVG% msg/s
) else (
    echo [ERROR] 无法获取基线版本吞吐量
    set BASELINE_AVG=0
)

echo.
goto :eof

REM =====================================================================
REM  PGO 测试
REM =====================================================================
:test_pgo
if not exist %PGO_EXE% (
    echo [ERROR] PGO 版本不存在: %PGO_EXE%
    echo 请先运行 build_pgo.bat 生成 PGO 版本
    exit /b 1
)

echo [测试 2/2] PGO 版本性能测试
echo   运行 %ITERATIONS% 次重放...
echo.

set PGO_TOTAL=0
set PGO_COUNT=0

for /l %%i in (1,1,%ITERATIONS%) do (
    echo   [%%i/%ITERATIONS%] 运行中...
    %PGO_EXE% %DATA_FILE% 0 2 16384 64 1 > temp_output.txt 2>&1

    REM 解析输出获取吞吐量
    for /f "tokens=*" %%a in ('findstr "msg/s" temp_output.txt') do (
        set LINE=%%a
        REM 提取数字部分
        for /f "tokens=2" %%b in ("!LINE!") do (
            set THROUGHPUT=%%b
            set /a PGO_TOTAL+=!THROUGHPUT!
            set /a PGO_COUNT+=1
        )
    )
)

if %PGO_COUNT% gtr 0 (
    set /a PGO_AVG=%PGO_TOTAL% / %PGO_COUNT%
    echo   PGO 版本平均吞吐量: %PGO_AVG% msg/s
) else (
    echo [ERROR] 无法获取 PGO 版本吞吐量
    set PGO_AVG=0
)

echo.
goto :eof

REM =====================================================================
REM  完整对比测试
REM =====================================================================
:test_all
call :test_baseline
call :test_pgo

echo.
echo ============================================================
echo  性能对比结果
echo ============================================================
echo.

if %BASELINE_AVG% gtr 0 (
    echo  基线版本: %BASELINE_AVG% msg/s
) else (
    echo  基线版本: 测试失败
)

if %PGO_AVG% gtr 0 (
    echo  PGO 版本: %PGO_AVG% msg/s
) else (
    echo  PGO 版本: 测试失败
)

if %BASELINE_AVG% gtr 0 if %PGO_AVG% gtr 0 (
    set /a IMPROVEMENT=(%PGO_AVG% - %BASELINE_AVG%) * 100 / %BASELINE_AVG%
    echo.
    echo  性能提升: !IMPROVEMENT!%%
)

echo.
echo ============================================================

REM 清理临时文件
del temp_output.txt 2>nul

endlocal