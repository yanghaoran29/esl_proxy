#!/usr/bin/env bash
# Build and run esl_proxy onboard smoke — no simpler Python/runtime at run time.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export ESL_PROXY_ROOT="$ROOT"
if [[ -d "${ROOT}/../simpler/simpler_setup" ]]; then
  export SIMPLER_ROOT="$(cd "${ROOT}/../simpler" && pwd)"
fi

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH must be set" >&2
  exit 1
fi

SKIP_BUILD=0
DEVICE_ID=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    -d|--device) DEVICE_ID="$2"; shift 2 ;;
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
  SWIMLANE_LEVEL="${ESL_PROXY_L2_SWIMLANE_LEVEL:-0}"
  if [[ "$SWIMLANE_LEVEL" != "0" ]]; then
    export ESL_PROXY_ENABLE_L2_SWIMLANE=1
  fi
  if [[ -n "${ESL_PROXY_ORCH_CASE:-}" ]]; then
    export ORCH_CASE="$ESL_PROXY_ORCH_CASE"
    echo "[esl_proxy] ORCH_CASE=$ORCH_CASE"
  fi
  bash cmake/onboard/build_aicpu.sh
  bash cmake/onboard/build_aicore.sh
  bash cmake/onboard/build_onboard_host.sh
fi

RUNNER="${ROOT}/build/onboard/host/esl_onboard_runner"
DISPATCHER="${ROOT}/build/onboard/aicpu/libesl_aicpu_dispatcher.so"
AICPU="${ROOT}/build/onboard/aicpu/libaicpu_kernel.so"
AICORE="${ROOT}/build/onboard/aicore/aicore_kernel.o"

for f in "$RUNNER" "$DISPATCHER" "$AICPU" "$AICORE"; do
  if [[ ! -f "$f" ]]; then
    echo "missing $f (run without --skip-build first)" >&2
    exit 1
  fi
done

"$RUNNER" -d "$DEVICE_ID" \
  --dispatcher "$DISPATCHER" \
  --aicpu "$AICPU" \
  --aicore "$AICORE"

# Raw l2_swimlane_records.json is not Perfetto format; runner also emits trace JSON
# when swimlane export succeeds. Re-run conversion here if the runner skipped it.
SWIMLANE_LEVEL="${ESL_PROXY_L2_SWIMLANE_LEVEL:-0}"
RAW_SWIMLANE="${ROOT}/l2_swimlane_records.json"
TRACE_SWIMLANE="${ROOT}/l2_swimlane_trace.json"
if [[ "$SWIMLANE_LEVEL" != "0" && -f "$RAW_SWIMLANE" && ! -f "$TRACE_SWIMLANE" ]]; then
  python3 "${ROOT}/tools/swimlane_to_perfetto.py" "$RAW_SWIMLANE" -o "$TRACE_SWIMLANE" || true
fi
