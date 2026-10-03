#!/usr/bin/env bash
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
compiler_dir="$experiment_root/../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129/bin"
mkdir -p "$experiment_root/build"
cc -O2 -Wall -Wextra -Werror -DHOST_TEST "$experiment_root/probes/gateway-maintenance.c" -o "$experiment_root/build/gateway-maintenance-host"
"$compiler_dir/mips-linux-gcc" -Os -Wall -Wextra -Werror -static -msoft-float "$experiment_root/probes/gateway-maintenance.c" -o "$experiment_root/build/gateway-maintenance"
"$compiler_dir/mips-linux-strip" "$experiment_root/build/gateway-maintenance"
