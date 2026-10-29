#!/usr/bin/env bash
set -euo pipefail
root=$(cd -P -- "$(dirname -- "$0")/../.." && pwd)
source "$root/scripts/toolchains.sh"
output="$root/build/${TARGET_BUILD_SUBDIR}gateway-core"
mkdir -p "$(dirname "$output")"
cc -O2 -g -std=gnu99 -Wall -Wextra -Werror "$root/scripts/core-capture/gateway-core.c" -o "$root/build/gateway-core-host"
"$TARGET_CC" -Os -std=gnu99 -Wall -Wextra -Werror -static -msoft-float "$root/scripts/core-capture/gateway-core.c" -o "$output"
"$TARGET_STRIP" "$output"
python3 "$root/scripts/verify-gateway-target.py" "$output"

"$TARGET_CC" -Os -std=gnu99 -Wall -Wextra -Werror -static -msoft-float "$root/scripts/core-capture/snapshot-daemon.c" "$root/scripts/core-capture/snapshot-writer.c" -o "$(dirname "$output")/gateway-snapshot"
"$TARGET_STRIP" "$(dirname "$output")/gateway-snapshot"
python3 "$root/scripts/verify-gateway-target.py" "$(dirname "$output")/gateway-snapshot"
