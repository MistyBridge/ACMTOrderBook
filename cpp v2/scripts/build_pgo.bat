@echo off
REM =====================================================================
REM  build_pgo.bat — PGO (Profile-Guided Optimization) 编译脚本
REM
REM  用法：
REM    build_pgo.bat gen    # 第一步：生成 profile 数据
REM    build_pgo.bat use    # 第二步：使用 profile 数据编译优化版本
REM    build_pgo.bat all    # 自动执行完整 PGO 流程
REM
REM  预期收益：+10% 吞吐量
REM =====================================================================

setlocal enabledelayedexpansion

set BUILD_DIR=build_pgo
set PROFILE_DIR=pgo_profiles
set DATA_FILE=../data/20220422/AX_sbe_szse_000001.log
set PGO_EXE=%BUILD_DIR%\Release\orderbook_v2.exe

echo.
echo ============================================================
echo  ACMTOrderBook v2.5 — PGO 编译优化
echo  预期收益：+10%% 吞吐量
echo ============================================================
echo.

REM 解析参数
set ACTION=%1
if "%ACTION%"=="" set ACTION=all

if "%ACTION%"=="gen" (
    goto :generate_profile
) else if "%ACTION%"=="use" (
    goto :use_profile
) else if "%ACTION%"=="all" (
    goto :full_pgo
) else (
    echo [ERROR] 未知参数: %ACTION%
    echo 用法: build_pgo.bat [gen^|use^|all]
    exit /b 1
)

REM =====================================================================
REM  第一步：生成 profile 数据
REM =====================================================================
:generate_profile
echo [步骤 1/3] 生成 profile 数据版本...
echo.

REM 清理旧的 profile 数据
if exist %PROFILE_DIR% (
    echo [INFO] 清理旧的 profile 数据...
    rmdir /s /q %PROFILE_DIR%
)
mkdir %PROFILE_DIR%

REM 配置 CMake（PGO 生成模式）
cmake -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 -DPGO_MODE=GEN -DPGO_PROFILE_DIR=%PROFILE_DIR%
if errorlevel 1 (
    echo [ERROR] CMake 配置失败
    exit /b 1
)

REM 编译
cmake --build %BUILD_DIR% --config Release --parallel 8
if errorlevel 1 (
    echo [ERROR] 编译失败
    exit /b 1
)

echo.
echo [步骤 2/3] 运行程序收集 profile 数据...
echo.

REM 运行程序收集 profile 数据
%PGO_EXE% %DATA_FILE% 0 2 16384 64 5
if errorlevel 1 (
    echo [ERROR] 程序运行失败
    exit /b 1
)

echo.
echo [INFO] 检查 profile 数据文件...

REM MSVC: .pgc 文件在可执行文件目录
REM 需要复制到 profile 目录
if exist %BUILD_DIR%\Release\*.pgc (
    echo [INFO] 找到 MSVC profile 数据文件
    copy %BUILD_DIR%\Release\*.pgc %PROFILE_DIR%\
    echo [INFO] Profile 数据已复制到 %PROFILE_DIR%
) else (
    echo [WARNING] 未找到 .pgc 文件
    echo [WARNING] 请检查可执行文件目录: %BUILD_DIR%\Release\
)

echo.

if "%ACTION%"=="gen" (
    echo ============================================================
    echo  第一步完成！
    echo  下一步：运行 build_pgo.bat use
    echo ============================================================
    goto :end
)

REM =====================================================================
REM  第二步：使用 profile 数据编译优化版本
REM =====================================================================
:use_profile
echo [步骤 3/3] 使用 profile 数据编译优化版本...
echo.

REM 检查 profile 数据
if not exist %PROFILE_DIR% (
    echo [ERROR] Profile 数据不存在，请先运行 build_pgo.bat gen
    exit /b 1
)

REM 配置 CMake（PGO 使用模式）
cmake -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 -DPGO_MODE=USE -DPGO_PROFILE_DIR=%PROFILE_DIR%
if errorlevel 1 (
    echo [ERROR] CMake 配置失败
    exit /b 1
)

REM 编译优化版本
cmake --build %BUILD_DIR% --config Release --parallel 8
if errorlevel 1 (
    echo [ERROR] 编译失败
    exit /b 1
)

echo.
echo ============================================================
echo  PGO 编译优化完成！
echo  优化版本：%PGO_EXE%
echo ============================================================
echo.

goto :end

REM =====================================================================
REM  完整 PGO 流程
REM =====================================================================
:full_pgo
call :generate_profile
call :use_profile

echo.
echo ============================================================
echo  PGO 优化完成！
echo.
echo  下一步：
echo  1. 运行性能测试验证收益
echo  2. 对比普通版本和 PGO 版本
echo ============================================================
echo.

:end
endlocal