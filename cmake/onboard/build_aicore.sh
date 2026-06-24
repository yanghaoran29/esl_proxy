#!/usr/bin/env bash
# Build esl_proxy onboard AICore kernel (aicore_execute + fake-FIN dispatch loop).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Self-contained: onboard platform headers live under platform/include/onboard.
ONBOARD_INC="${ROOT}/esl_proxy/include/onboard"
BUILD_DIR="${ROOT}/build/onboard/aicore"
AICORE_SRC="${ROOT}/esl_proxy/src/onboard"

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

CUSTOM_INCLUDES="${ONBOARD_INC}"

cmake -B "$BUILD_DIR" -S "${ROOT}/cmake/onboard/aicore" \
  -DBISHENG_CC="$BISHENG_CC" \
  -DBISHENG_LD="$BISHENG_LD" \
  -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
  -DCUSTOM_SOURCE_DIRS="${AICORE_SRC}"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Built: ${BUILD_DIR}/aicore_kernel.o"
