#!/usr/bin/env bash
# Build esl_proxy onboard AICPU kernel (.so).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEFAULT_SIMPLER="$(cd "$ROOT/../simpler" 2>/dev/null && pwd || true)"
SIMPLER_ROOT="${SIMPLER_ROOT:-$DEFAULT_SIMPLER}"
if [[ -z "$SIMPLER_ROOT" || ! -d "$SIMPLER_ROOT" ]]; then
  ALT="$(cd "$ROOT/../../simpler" 2>/dev/null && pwd || true)"
  if [[ -n "$ALT" && -d "$ALT" ]]; then
    SIMPLER_ROOT="$ALT"
  fi
fi
BUILD_DIR="${ROOT}/build/onboard/aicpu"
ESL_CORE="${ROOT}/esl_proxy"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH is not set" >&2
  exit 1
fi
if [[ -z "$SIMPLER_ROOT" || ! -d "$SIMPLER_ROOT" ]]; then
  echo "SIMPLER_ROOT not found (expected sibling ../simpler)" >&2
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

HOST_BUILD_GRAPH="${SIMPLER_ROOT}/src/a2a3/runtime"
RUNTIME_INC="${SIMPLER_ROOT}/src/a2a3/runtime/tensormap_and_ringbuffer/runtime"
PLATFORM_COMMON="${SIMPLER_ROOT}/src/a2a3/platform/include"
PLATFORM_COMMON_SHARED="${SIMPLER_ROOT}/src/common/platform/include"
PLATFORM_ONBOARD_AICPU="${SIMPLER_ROOT}/src/common/platform/onboard/aicpu"

CUSTOM_INCLUDES="${ROOT}/platform/include"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${ESL_CORE}/include"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${ROOT}/tests/onboard/smoke"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${ROOT}/platform/a2a3/aicpu"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${PLATFORM_COMMON}"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${PLATFORM_COMMON_SHARED}"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${PLATFORM_ONBOARD_AICPU}"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${HOST_BUILD_GRAPH}"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${RUNTIME_INC}"

CUSTOM_SOURCES="${ROOT}/platform/a2a3/aicpu"
CUSTOM_SOURCES="${CUSTOM_SOURCES};${ROOT}/platform/a2a3/aicpu/esl_src"

export SIMPLER_DISABLE_WARNINGS_AS_ERRORS=1

cmake -B "$BUILD_DIR" -S "${ROOT}/platform/cmake/aicpu" \
  -DCMAKE_CXX_COMPILER="$CROSS_CXX" \
  -DCMAKE_C_COMPILER="$CROSS_CC" \
  -DCMAKE_C_FLAGS="-DESL_PROXY_ONBOARD -Wno-error -w" \
  -DCMAKE_CXX_FLAGS="-DESL_PROXY_ONBOARD -Wno-error -w" \
  -DSIMPLER_ROOT="$SIMPLER_ROOT" \
  -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
  -DCUSTOM_SOURCE_DIRS="${CUSTOM_SOURCES}" \
  -DESCEND_HOME_PATH="$ASCEND_HOME_PATH"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Built: ${BUILD_DIR}/libaicpu_kernel.so"
