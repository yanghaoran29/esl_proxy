#!/usr/bin/env bash
# Build esl_proxy onboard AICPU kernel (.so).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Self-contained: all onboard code lives under esl_proxy/{include,src}/onboard.
# No external ../simpler checkout and no SIMPLER_ROOT env var.
ESL_CORE="${ROOT}/esl_proxy"
ONBOARD_INC="${ESL_CORE}/include/onboard"
ONBOARD_SRC="${ESL_CORE}/src/onboard"
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

# Queue nodes may use a different ASCEND_HOME_PATH than the last configure host.
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cached="$(grep -m1 '^CMAKE_CXX_COMPILER:FILEPATH=' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
  if [[ -n "$cached" && "$cached" != "$CROSS_CXX" ]]; then
    rm -rf "$BUILD_DIR"
  fi
fi

# Orchestration case: case_orch.c includes cases/${ORCH_CASE} (which defines
# aicpu_orchestration_entry). Override with ORCH_CASE=... env. Defaults to the
# real paged-attention manual-scope case (1920 tasks).
ORCH_CASE="${ORCH_CASE:-paged_attention_unroll_manual_scope.h}"
QWEN3_SPMD_TIER="${QWEN3_SPMD_TIER:-2}"

CUSTOM_INCLUDES="${ONBOARD_INC};${ESL_CORE}/include;${ESL_CORE}/cases;${ONBOARD_SRC};${ONBOARD_SRC}/l2_swimlane"

# Sources are flat in src/onboard. Build the AICPU kernel file list = everything
# in src/onboard EXCEPT the aicore / host / dispatcher sources, plus the core
# scheduler files from esl_proxy/src.
NOT_AICPU="aicore_kernel.cpp host_runner.c host_onboard.c aicpu_dispatcher.c"
SRC_FILES=""
for f in "$ONBOARD_SRC"/*.c "$ONBOARD_SRC"/*.cpp; do
  [[ -e "$f" ]] || continue
  base="$(basename "$f")"
  skip=0
  for e in $NOT_AICPU; do [[ "$base" == "$e" ]] && skip=1 && break; done
  [[ $skip -eq 0 ]] && SRC_FILES="${SRC_FILES:+$SRC_FILES;}$f"
done
for f in aicpu_runtime cutter dispatch dispatch_payload shm; do SRC_FILES="${SRC_FILES};${ESL_CORE}/src/${f}.c"; done

export SIMPLER_DISABLE_WARNINGS_AS_ERRORS=1
ONBOARD_LOG_FLAGS="-DWORKER_LOG=0 -DMAIN_LOG=0"

cmake -B "$BUILD_DIR" -S "${ROOT}/platform/cmake/aicpu" \
  -DCMAKE_CXX_COMPILER="$CROSS_CXX" \
  -DCMAKE_C_COMPILER="$CROSS_CC" \
  -DCMAKE_C_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} -Wno-error -w" \
  -DCMAKE_CXX_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} -Wno-error -w" \
  -DESL_ONBOARD_DIR="$ONBOARD_SRC" \
  -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
  -DCUSTOM_SOURCE_FILES="${SRC_FILES}" \
  -DESCEND_HOME_PATH="$ASCEND_HOME_PATH"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Built: ${BUILD_DIR}/libaicpu_kernel.so"
