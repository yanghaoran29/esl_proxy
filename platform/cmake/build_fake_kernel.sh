#!/usr/bin/env bash
# Build standalone fake_kernel.o (user kernel stub for ChipCallable children).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="${ROOT}/platform/a2a3/aicore/fake_kernel.cpp"
OUT_DIR="${ROOT}/build/onboard/aicore"
OUT="${OUT_DIR}/fake_kernel.o"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH is not set" >&2
  exit 1
fi

BISHENG_CC="$(command -v ccec || echo "${ASCEND_HOME_PATH}/compiler/ccec_compiler/bin/ccec")"
BISHENG_LD="$(command -v ld.lld || echo "${ASCEND_HOME_PATH}/compiler/ccec_compiler/bin/ld.lld")"
mkdir -p "$OUT_DIR"

FLAGS=(-c -O3 -g -x cce -Wall -std=c++17 --cce-aicore-only
  -mllvm -cce-aicore-stack-size=0x8000
  -mllvm -cce-aicore-function-stack-size=0x8000
  -mllvm -cce-aicore-record-overflow=false
  -mllvm -cce-aicore-addr-transform
  -mllvm -cce-aicore-dcci-insert-for-scalar=false)

OBJ_AIC="${OUT_DIR}/fake_kernel_aic.o"
"$BISHENG_CC" "${FLAGS[@]}" --cce-aicore-arch=dav-c220-cube -o "$OBJ_AIC" "$SRC"
# Stub kernel: AIC-only object (same symbol in AIC+AIV would duplicate at link time).
cp "$OBJ_AIC" "$OUT"
echo "Built: $OUT"
