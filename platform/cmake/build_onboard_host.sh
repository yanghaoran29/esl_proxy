#!/usr/bin/env bash
# Build esl_proxy standalone onboard host runner (no simpler runtime).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT}/build/onboard/host"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH is not set" >&2
  exit 1
fi
if [[ -f "${ASCEND_HOME_PATH}/bin/setenv.bash" ]]; then
  # shellcheck disable=SC1090
  source "${ASCEND_HOME_PATH}/bin/setenv.bash"
fi

cmake -B "$BUILD_DIR" -S "${ROOT}/platform/a2a3/host" \
  -DCMAKE_BUILD_TYPE=Release \
  -DASCEND_HOME_PATH="$ASCEND_HOME_PATH"

cmake --build "$BUILD_DIR" -j"$(nproc)"
echo "Built: ${BUILD_DIR}/esl_onboard_runner"
