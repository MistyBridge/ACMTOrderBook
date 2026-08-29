#!/usr/bin/env bash
# run_pgo.sh — 可复现的 PGO（Profile-Guided Optimization）两遍构建脚本。
#
# 流程:
#   1) PGO_GEN : 插桩编译 orderbook_v2
#   2) RUN     : 用给定行情数据跑一次, 生成 .gcda profile
#   3) PGO_USE : 用 profile 重编 orderbook_v2
#   4) 打印 基线 vs PGO 的吞吐对比 (system T1 + engine T2)
#
# 用法:
#   ./scripts/run_pgo.sh <data_file> [recurse_build_dir]
#     data_file   : 行情 .log。可先用 tools/gen_market_data.py 生成合成数据,
#                   或提供真实的逐笔数据(推荐, 真实热点分布才有真收益)。
#     示例: ./scripts/run_pgo.sh data_synth.log
#
# 说明:
#   - 需要 cmake + GNU 编译器 (gcc/g++), 支持 -fprofile-generate/-fprofile-use。
#   - 实测(合成数据 200k msgs, MinGW g++ 8.1): T2 引擎纯处理与 T1 系统端到端
#     均在运行噪声内 (~±1%), 无明显收益; 原因: 引擎已 -O3 -flto + LIKELY/UNLIKELY
#     分支提示, PGO 主要对分支密集/未显式标注的代码收益大。真实数据热分布可能更有利。
set -euo pipefail

DATA="${1:?usage: run_pgo.sh <data_file>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/cpp v2"
BUILD_ROOT="${2:-$REPO/build-pgo}"
PROF_DIR="${PROF_DIR:-$REPO/pgo_profiles}"
NPROC="$(nproc 2>/dev/null || echo 8)"

cmake_cfg() { # $1=builddir $2=mode
  cmake -S "$SRC" -B "$1" -DCMAKE_BUILD_TYPE=Release \
        -DUSE_FLAT_HASHMAP=0 -DBUILD_TESTING=OFF \
        -DPGO_MODE="$2" -DPGO_PROFILE_DIR="$PROF_DIR"
  cmake --build "$1" --target orderbook_v2 --parallel "$NPROC"
}

echo "[PGO] 基线编译 (无 profile)"
cmake_cfg "$BUILD_ROOT/base" OFF
BASE_T1="$($BUILD_ROOT/base/orderbook_v2 "$DATA" 0 2 16384 64 1 2>/dev/null | grep '^Time:' || true)"
BASE_T2="$($BUILD_ROOT/base/orderbook_v2 "$DATA" 0 2 16384 64 1 2>/dev/null | grep 'Throughput(engine)' || true)"

echo "[PGO] 插桩编译 (GEN) + 运行生成 profile"
cmake_cfg "$BUILD_ROOT/gen" GEN
"$BUILD_ROOT/gen/orderbook_v2" "$DATA" 0 2 16384 64 1 >/dev/null 2>&1 || true
[ -d "$PROF_DIR" ] || { echo "!! no .gcda generated (need real data?)"; exit 1; }

echo "[PGO] 用 profile 重编 (USE)"
cmake_cfg "$BUILD_ROOT/use" USE
USE_T1="$($BUILD_ROOT/use/orderbook_v2 "$DATA" 0 2 16384 64 1 2>/dev/null | grep '^Time:' || true)"
USE_T2="$($BUILD_ROOT/use/orderbook_v2 "$DATA" 0 2 16384 64 1 2>/dev/null | grep 'Throughput(engine)' || true)"

echo "=== 基线 ==="
echo "  $BASE_T1"; echo "  $BASE_T2"
echo "=== PGO(USE) ==="
echo "  $USE_T1"; echo "  $USE_T2"
