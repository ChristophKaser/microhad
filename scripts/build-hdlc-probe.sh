#!/usr/bin/env bash
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
compiler_dir="$experiment_root/../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129/bin"
mkdir -p "$experiment_root/build"
"$compiler_dir/mips-linux-gcc" -Os -static -msoft-float \
    -o "$experiment_root/build/hdlc-unix-probe" "$experiment_root/probes/unix-hdlc-probe.c"
"$compiler_dir/mips-linux-strip" "$experiment_root/build/hdlc-unix-probe"
