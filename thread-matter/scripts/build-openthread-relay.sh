#!/usr/bin/env bash
# Small test-only relay; OpenThread and all radio traffic remain simulated.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
source "$experiment_root/scripts/toolchains.sh"
output="$experiment_root/build/openthread-unix"
mkdir -p "$output"
for artifact in sim-relay drop-test-cache; do
    source_file=openthread-sim-relay
    if [ "$artifact" = drop-test-cache ]; then source_file=drop-test-cache; fi
case "${1:-mips}" in
    host)
        "$HOST_CC" -std=gnu99 -Wall -Wextra -Werror -O2 \
            "$experiment_root/thread-matter/tests/helpers/$source_file.c" -o "$output/${artifact}-host"
        ;;
    mips)
        "$TARGET_CC" -std=gnu99 -Wall -Wextra -Werror -Os -static -msoft-float \
            "$experiment_root/thread-matter/tests/helpers/$source_file.c" -o "$output/${artifact}-mips"
        "$TARGET_STRIP" "$output/${artifact}-mips"
        ;;
    *) echo 'Usage: build-openthread-relay.sh [host|mips]' >&2; exit 2 ;;
esac
done
