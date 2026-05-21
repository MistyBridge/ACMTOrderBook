@echo off
setlocal

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >/dev/null 2>&1

echo Compiling benchmark with ankerl::unordered_dense...
cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 ^
   /I"..\third_party\unordered_dense\include" ^
   /I"..\third_party\robin-hood-hashing\src\include" ^
   /DUSE_ROBIN_HOOD /DUSE_UNORDERED_DENSE ^
   /Febenchmark_ankerl.exe hashmap_benchmark.cpp

if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS]
    benchmark_ankerl.exe
) else (
    echo [FAILED]
)

endlocal
