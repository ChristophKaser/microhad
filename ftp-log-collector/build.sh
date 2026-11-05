#!/usr/bin/env bash
set -euo pipefail
root=$(cd -P -- "$(dirname -- "$0")/.." && pwd)
source "$root/scripts/toolchains.sh"
output="$root/build/${TARGET_BUILD_SUBDIR}ftp-log-collector"
mkdir -p "$(dirname "$output")"
"$TARGET_CC" -Os -std=gnu99 -Wall -Wextra -Werror -static -msoft-float -pthread \
    "$root/ftp-log-collector/main.c" "$root/ftp-log-collector/ftp.c" -o "$output"
"$TARGET_STRIP" "$output"
python3 "$root/scripts/verify-gateway-target.py" "$output"
