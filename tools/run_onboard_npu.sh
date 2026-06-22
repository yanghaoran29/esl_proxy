#!/usr/bin/env bash
set -euo pipefail
source "$ASCEND_HOME_PATH/bin/setenv.bash"
ROOT=/data/y00955915/Desktop/esl_proxy_main/esl_proxy
cd "$ROOT"
bash tools/run_onboard.sh --skip-build
