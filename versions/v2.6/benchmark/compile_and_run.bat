@echo off
setlocal

echo.
echo =====================================================================
echo   哈希表基准测试编译脚本
echo =====================================================================
echo.

REM 设置 MSVC 环境
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] 无法初始化 MSVC 环境
    exit /b 1
)

REM 清理旧文件
del /q *.exe *.obj 2>nul

echo [1/3] 编译 std::unordered_map 基准测试...
cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /Febenchmark_std.exe hashmap_benchmark_simple.cpp >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] 编译失败
    exit /b 1
)
echo [OK] benchmark_std.exe

echo.
echo [2/3] 编译 ankerl::unordered_dense 基准测试...
cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /I..\third_party\unordered_dense\include /DUSE_UNORDERED_DENSE /Fe:benchmark_ankerl.exe hashmap_benchmark.cpp >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] 编译失败，尝试简化版本...
    cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /I..\third_party\unordered_dense\include /Fetest_ankerl.exe test_ankerl.cpp >nul 2>&1
    if %ERRORLEVEL% NEQ 0 (
        echo [ERROR] 简化版本也编译失败
        exit /b 1
    )
    echo [OK] test_ankerl.exe
    goto :run_simple
)
echo [OK] benchmark_ankerl.exe

echo.
echo [3/3] 运行基准测试...
echo.
echo =====================================================================
echo   std::unordered_map 测试结果
echo =====================================================================
benchmark_std.exe

echo.
echo =====================================================================
echo   ankerl::unordered_dense 测试结果
echo =====================================================================
benchmark_ankerl.exe

goto :end

:run_simple
echo.
echo =====================================================================
echo   std::unordered_map 测试结果
echo =====================================================================
benchmark_std.exe

echo.
echo =====================================================================
echo   ankerl::unordered_dense 测试结果
echo =====================================================================
test_ankerl.exe

:end
echo.
echo =====================================================================
echo   测试完成
echo =====================================================================

endlocal