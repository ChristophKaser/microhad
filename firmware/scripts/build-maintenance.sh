#!/usr/bin/env bash
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
source "$experiment_root/scripts/toolchains.sh"
mkdir -p "$experiment_root/build"
"$HOST_CC" -O2 -Wall -Wextra -Werror -DHOST_TEST "$experiment_root/firmware/tools/gateway-maintenance.c" -o "$experiment_root/build/gateway-maintenance-host"
"$TARGET_CC" -Os -Wall -Wextra -Werror -static -msoft-float "$experiment_root/firmware/tools/gateway-maintenance.c" -o "$experiment_root/build/gateway-maintenance"
"$TARGET_STRIP" "$experiment_root/build/gateway-maintenance"
