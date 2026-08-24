@echo off
setlocal

set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set SRC="D:\开发\多线程订单簿撮合引擎\ACMTOrderBook-master\ACMTOrderBook-master\cpp v2"

cd /d %SRC%

echo.
echo ============================================================
echo  Step 1: Configure CMake
echo ============================================================
rmdir /s /q build 2>nul
%CMAKE% -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_BENCHMARK=ON -DUSE_UNORDERED_DENSE=ON
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configure failed: %ERRORLEVEL%
    exit /b 1
)

echo.
echo ============================================================
echo  Step 2: Build hashmap_benchmark
echo ============================================================
%CMAKE% --build build --config Release --target hashmap_benchmark --parallel 8
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed: %ERRORLEVEL%
    exit /b 1
)

echo.
echo ============================================================
echo  Step 3: Run benchmark
echo ============================================================
build\Release\hashmap_benchmark.exe

echo.
echo ============================================================
echo  Done!
echo ============================================================

endlocal