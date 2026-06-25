#!/usr/bin/env bash
# Build esl_proxy onboard AICore kernel (platform entry + algorithm executor).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ONBOARD_INC="${ROOT}/esl_proxy/include/platform/onboard"
BUILD_DIR="${ROOT}/build/onboard/aicore"
AICORE_ENTRY="${ROOT}/esl_proxy/src/platform/onboard/aicore_entry.cpp"
AICORE_EXECUTOR="${ROOT}/esl_proxy/src/platform/onboard/aicore_executor.cpp"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH is not set" >&2
  exit 1
fi
if [[ ! -d "$ONBOARD_INC" ]]; then
  echo "onboard platform headers missing: $ONBOARD_INC" >&2
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

CUSTOM_INCLUDES="${ONBOARD_INC};${ROOT}/esl_proxy/include/algorithm;${ROOT}/esl_proxy/include/swimlane;${ROOT}/esl_proxy/include/platform/sim"
ESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE:-0}"

cmake -B "$BUILD_DIR" -S "${ROOT}/build/cmake/aicore" \
  -DBISHENG_CC="$BISHENG_CC" \
  -DBISHENG_LD="$BISHENG_LD" \
  -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
  -DAICORE_ENTRY_SRC="${AICORE_ENTRY}" \
  -DAICORE_EXECUTOR_SRC="${AICORE_EXECUTOR}" \
  -DESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE}"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Built: ${BUILD_DIR}/aicore_kernel.o"
