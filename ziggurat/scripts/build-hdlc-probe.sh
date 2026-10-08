#!/usr/bin/env bash
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
source "$experiment_root/scripts/toolchains.sh"
mkdir -p "$experiment_root/build"
"$TARGET_CC" -Os -static -msoft-float \
    -o "$experiment_root/build/hdlc-unix-probe" "$experiment_root/ziggurat/tests/helpers/unix-hdlc-probe.c"
"$TARGET_STRIP" "$experiment_root/build/hdlc-unix-probe"
