#!/usr/bin/env bash
# Build patched simpler host_runtime from esl_proxy_main/simpler (not toolkit namespace).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEFAULT_SIMPLER="$(cd "$ROOT/../simpler" 2>/dev/null && pwd || true)"
SIMPLER_ROOT="${SIMPLER_ROOT:-$DEFAULT_SIMPLER}"
RUNTIME_NAME="${RUNTIME_NAME:-tensormap_and_ringbuffer}"
BUILD_DIR="${SIMPLER_ROOT}/build/cache/a2a3/onboard/${RUNTIME_NAME}/host"
OUT_DIR="${SIMPLER_ROOT}/build/lib/a2a3/onboard/${RUNTIME_NAME}"
HOST_CMAKE="${SIMPLER_ROOT}/src/a2a3/platform/onboard/host"
RUNTIME_DIR="${SIMPLER_ROOT}/src/a2a3/runtime/${RUNTIME_NAME}"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH is not set" >&2
  exit 1
fi
if [[ -f "${ASCEND_HOME_PATH}/bin/setenv.bash" ]]; then
  # shellcheck disable=SC1090
  source "${ASCEND_HOME_PATH}/bin/setenv.bash"
fi
if [[ -z "${PTO_ISA_ROOT:-}" ]]; then
  echo "PTO_ISA_ROOT is not set" >&2
  exit 1
fi
if [[ ! -d "$SIMPLER_ROOT" ]]; then
  echo "SIMPLER_ROOT not found: $SIMPLER_ROOT" >&2
  exit 1
fi

INC="${RUNTIME_DIR}/runtime;${RUNTIME_DIR}/common;${RUNTIME_DIR}/.."
SRC="${RUNTIME_DIR}/host;${RUNTIME_DIR}/runtime/shared;${RUNTIME_DIR}/orchestration"
LOG_DIR="${SIMPLER_ROOT}/build/cache/simpler_log"
LOG_SO="${SIMPLER_ROOT}/build/lib/libsimpler_log.so"

if [[ ! -f "$LOG_SO" ]]; then
  mkdir -p "$LOG_DIR"
  cmake -B "$LOG_DIR" -S "${SIMPLER_ROOT}/src/common/log" \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "$LOG_DIR" -j"$(nproc)"
  mkdir -p "${SIMPLER_ROOT}/build/lib"
  cp -f "$LOG_DIR/libsimpler_log.so" "$LOG_SO"
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$OUT_DIR"
cmake -B "$BUILD_DIR" -S "$HOST_CMAKE" \
  -DCMAKE_BUILD_TYPE=Release \
  -DASCEND_HOME_PATH="$ASCEND_HOME_PATH" \
  -DCUSTOM_INCLUDE_DIRS="${INC}" \
  -DCUSTOM_SOURCE_DIRS="${SRC}" \
  -DESCEND_HOME_PATH="$ASCEND_HOME_PATH"
cmake --build "$BUILD_DIR" -j"$(nproc)"
cp -f "${BUILD_DIR}/libhost_runtime.so" "${OUT_DIR}/libhost_runtime.so"
strip -s "${OUT_DIR}/libhost_runtime.so" || true
echo "Built: ${OUT_DIR}/libhost_runtime.so"
