#!/usr/bin/env bash
# Build and run esl_proxy onboard smoke — no simpler Python/runtime at run time.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH must be set" >&2
  exit 1
fi

SKIP_BUILD=0
DEVICE_ID=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    -d) DEVICE_ID="$2"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [-d device_id] [--skip-build]"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [[ -n "${TASK_DEVICE:-}" ]]; then
  DEVICE_ID="$TASK_DEVICE"
fi

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  bash platform/cmake/build_aicpu.sh
  bash platform/cmake/build_aicore.sh
  bash platform/cmake/build_onboard_host.sh
fi

RUNNER="${ROOT}/build/onboard/host/esl_onboard_runner"
DISPATCHER="${ROOT}/build/onboard/aicpu/libsimpler_aicpu_dispatcher.so"
AICPU="${ROOT}/build/onboard/aicpu/libaicpu_kernel.so"
AICORE="${ROOT}/build/onboard/aicore/aicore_kernel.o"

for f in "$RUNNER" "$DISPATCHER" "$AICPU" "$AICORE"; do
  if [[ ! -f "$f" ]]; then
    echo "missing $f (run without --skip-build first)" >&2
    exit 1
  fi
done

exec "$RUNNER" -d "$DEVICE_ID" \
  --dispatcher "$DISPATCHER" \
  --aicpu "$AICPU" \
  --aicore "$AICORE"
