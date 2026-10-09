#!/usr/bin/env bash
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
source "$experiment_root/scripts/toolchains.sh"
output="$experiment_root/build/${TARGET_BUILD_SUBDIR}hdlc-unix-probe"
mkdir -p "$(dirname "$output")"
"$TARGET_CC" -Os -static -msoft-float \
    -o "$output" "$experiment_root/ziggurat/tests/helpers/unix-hdlc-probe.c"
"$TARGET_STRIP" "$output"
python3 "$experiment_root/scripts/verify-gateway-target.py" "$output"
