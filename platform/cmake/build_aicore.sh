#!/usr/bin/env bash
# Build esl_proxy onboard AICore kernel (aicore_execute + fake-FIN dispatch loop).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEFAULT_SIMPLER="$(cd "$ROOT/../simpler" 2>/dev/null && pwd || true)"
SIMPLER_ROOT="${SIMPLER_ROOT:-$DEFAULT_SIMPLER}"
BUILD_DIR="${ROOT}/build/onboard/aicore"
AICORE_SRC="${ROOT}/platform/a2a3/aicore"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH is not set" >&2
  exit 1
fi
if [[ -z "$SIMPLER_ROOT" || ! -d "$SIMPLER_ROOT" ]]; then
  echo "SIMPLER_ROOT not found" >&2
  exit 1
fi

BISHENG_CC="$(command -v ccec || echo "${ASCEND_HOME_PATH}/compiler/ccec_compiler/bin/ccec")"
BISHENG_LD="$(command -v ld.lld || echo "${ASCEND_HOME_PATH}/compiler/ccec_compiler/bin/ld.lld")"
if [[ ! -x "$BISHENG_CC" || ! -x "$BISHENG_LD" ]]; then
  echo "ccec/ld.lld not found under ASCEND_HOME_PATH" >&2
  exit 1
fi

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cached="$(grep -m1 '^BISHENG_CC:FILEPATH=' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
  if [[ -n "$cached" && "$cached" != "$BISHENG_CC" ]]; then
    rm -rf "$BUILD_DIR"
  fi
fi

RUNTIME="${SIMPLER_ROOT}/src/a2a3/runtime/tensormap_and_ringbuffer"
PLATFORM_INC="${SIMPLER_ROOT}/src/a2a3/platform/include"
ONBOARD_AICORE_INC="${SIMPLER_ROOT}/src/a2a3/platform/onboard/aicore"
COMMON_INC="${SIMPLER_ROOT}/src/common/platform/include"
COMMON_TASK="${SIMPLER_ROOT}/src/common/task_interface"
COMMON_LOG="${SIMPLER_ROOT}/src/common/log/include"

CUSTOM_INCLUDES="${ONBOARD_AICORE_INC};${PLATFORM_INC};${COMMON_INC};${COMMON_TASK};${COMMON_LOG}"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${RUNTIME}/runtime;${RUNTIME}/common;${RUNTIME}"
CUSTOM_INCLUDES="${CUSTOM_INCLUDES};${AICORE_SRC}"

cmake -B "$BUILD_DIR" -S "${ROOT}/platform/cmake/aicore" \
  -DBISHENG_CC="$BISHENG_CC" \
  -DBISHENG_LD="$BISHENG_LD" \
  -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
  -DCUSTOM_SOURCE_DIRS="${AICORE_SRC}"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Built: ${BUILD_DIR}/aicore_kernel.o"
