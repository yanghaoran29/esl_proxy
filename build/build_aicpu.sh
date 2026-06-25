#!/usr/bin/env bash
# Build esl_proxy onboard AICPU kernel (.so).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESL_CORE="${ROOT}/esl_proxy"
ONBOARD_INC="${ESL_CORE}/include/platform/onboard"
ONBOARD_SRC="${ESL_CORE}/src/platform/onboard"
ALGO_SRC="${ESL_CORE}/src/algorithm"
SWIMLANE_SRC="${ESL_CORE}/src/swimlane"
BUILD_DIR="${ROOT}/build/onboard/aicpu"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH is not set" >&2
  exit 1
fi
if [[ ! -d "$ONBOARD_INC" ]]; then
  echo "onboard platform headers missing: $ONBOARD_INC" >&2
  exit 1
fi

CROSS_CXX="${ASCEND_HOME_PATH}/tools/hcc/bin/aarch64-target-linux-gnu-g++"
if [[ ! -x "$CROSS_CXX" ]]; then
  CROSS_CXX="$(command -v aarch64-target-linux-gnu-g++ || true)"
fi
CROSS_CC="${CROSS_CXX/g++/gcc}"
if [[ ! -x "$CROSS_CC" ]]; then
  CROSS_CC="$(command -v aarch64-target-linux-gnu-gcc || true)"
fi
if [[ -z "$CROSS_CXX" || -z "$CROSS_CC" ]]; then
  echo "aarch64 cross compiler not found" >&2
  exit 1
fi

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cached="$(grep -m1 '^CMAKE_CXX_COMPILER:FILEPATH=' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
  if [[ -n "$cached" && "$cached" != "$CROSS_CXX" ]]; then
    rm -rf "$BUILD_DIR"
  fi
fi

ORCH_CASE="${ORCH_CASE:-paged_attention_unroll_manual_scope.h}"
QWEN3_SPMD_TIER="${QWEN3_SPMD_TIER:-0}"
ESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE:-0}"

CUSTOM_INCLUDES="${ONBOARD_INC};${ESL_CORE}/include/algorithm;${ESL_CORE}/include/platform;${ESL_CORE}/include/swimlane;${ESL_CORE}/include/platform/sim;${ESL_CORE}/cases"
SWIMLANE_FLAGS=""
if [[ "$ESL_PROXY_ENABLE_L2_SWIMLANE" != "0" ]]; then
  SWIMLANE_FLAGS="-DESL_PROXY_ENABLE_L2_SWIMLANE=1"
fi

SRC_FILES="${ONBOARD_SRC}/npu_hal.c"
SRC_FILES="${SRC_FILES};${ONBOARD_SRC}/onboard_log.c"
SRC_FILES="${SRC_FILES};${ONBOARD_SRC}/onboard_trace.c"
SRC_FILES="${SRC_FILES};${ONBOARD_SRC}/tools.c"
SRC_FILES="${SRC_FILES};${ONBOARD_SRC}/aicpu_platform_init.c"
SRC_FILES="${SRC_FILES};${ALGO_SRC}/aicore_handshake.c"
SRC_FILES="${SRC_FILES};${ALGO_SRC}/aicore_bridge.c"
SRC_FILES="${SRC_FILES};${ALGO_SRC}/aicpu_runtime.c"
SRC_FILES="${SRC_FILES};${ALGO_SRC}/cutter.c"
SRC_FILES="${SRC_FILES};${ALGO_SRC}/dispatch.c"
SRC_FILES="${SRC_FILES};${ALGO_SRC}/shm.c"
if [[ "$ESL_PROXY_ENABLE_L2_SWIMLANE" != "0" ]]; then
  SRC_FILES="${SRC_FILES};${SWIMLANE_SRC}/swimlane_aicpu.cpp"
fi

export SIMPLER_DISABLE_WARNINGS_AS_ERRORS=1
ONBOARD_LOG_FLAGS="-DWORKER_LOG=0 -DMAIN_LOG=0"

cmake -B "$BUILD_DIR" -S "${ROOT}/build/cmake/aicpu" \
  -DCMAKE_CXX_COMPILER="$CROSS_CXX" \
  -DCMAKE_C_COMPILER="$CROSS_CC" \
  -DCMAKE_C_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} ${SWIMLANE_FLAGS} -Wno-error -w" \
  -DCMAKE_CXX_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} ${SWIMLANE_FLAGS} -Wno-error -w" \
  -DESL_ONBOARD_DIR="$ONBOARD_SRC" \
  -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
  -DCUSTOM_SOURCE_FILES="${SRC_FILES}" \
  -DESCEND_HOME_PATH="$ASCEND_HOME_PATH"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Built: ${BUILD_DIR}/libaicpu_kernel.so"
