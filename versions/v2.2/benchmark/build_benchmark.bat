@echo off
REM 基准测试编译脚本
REM 需要 MSVC 2022 Build Tools

setlocal

REM 设置 MSVC 环境
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

REM 编译基准测试（仅 std::unordered_map）
echo Compiling benchmark...
cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 ^
   /I"..\third_party\robin-hood-hashing\src\include" ^
   /I"..\third_party\unordered_dense\include" ^
   /DUSE_ROBIN_HOOD ^
   /DUSE_UNORDERED_DENSE ^
   /Febenchmark.exe ^
   hashmap_benchmark.cpp

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Run: benchmark.exe
    echo.
) else (
    echo.
    echo Build failed!
    echo.
)

endlocal
