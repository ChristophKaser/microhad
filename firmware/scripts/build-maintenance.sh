#!/usr/bin/env bash
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
source "$experiment_root/scripts/toolchains.sh"
mkdir -p "$experiment_root/build"
"$HOST_CC" -O2 -Wall -Wextra -Werror -DHOST_TEST "$experiment_root/firmware/tools/gateway-maintenance.c" -o "$experiment_root/build/gateway-maintenance-host"
target_output="$experiment_root/build/${TARGET_BUILD_SUBDIR}gateway-maintenance"
mkdir -p "$(dirname "$target_output")"
"$TARGET_CC" -Os -Wall -Wextra -Werror -static -msoft-float "$experiment_root/firmware/tools/gateway-maintenance.c" -o "$target_output"
"$TARGET_STRIP" "$target_output"
python3 "$experiment_root/scripts/verify-gateway-target.py" "$target_output"
