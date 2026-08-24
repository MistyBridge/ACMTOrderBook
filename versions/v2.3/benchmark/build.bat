@echo off
setlocal

REM 设置 MSVC 环境
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

echo.
echo Compiling benchmark...
echo.

cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /Febenchmark_simple.exe hashmap_benchmark_simple.cpp

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] Build successful!
    echo Run: benchmark_simple.exe
    echo.
) else (
    echo.
    echo [FAILED] Build failed!
    echo.
)

endlocal