#!/usr/bin/env bash
# Small test-only relay; OpenThread and all radio traffic remain simulated.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
output="$experiment_root/build/openthread-unix"
mkdir -p "$output"
for artifact in sim-relay drop-test-cache; do
    source_file=openthread-sim-relay
    if [ "$artifact" = drop-test-cache ]; then source_file=drop-test-cache; fi
case "${1:-mips}" in
    host)
        gcc -std=gnu99 -Wall -Wextra -Werror -O2 \
            "$experiment_root/probes/$source_file.c" -o "$output/${artifact}-host"
        ;;
    mips)
        rsdk="$experiment_root/../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129"
        "$rsdk/bin/mips-linux-gcc" -std=gnu99 -Wall -Wextra -Werror -Os -static -msoft-float \
            "$experiment_root/probes/$source_file.c" -o "$output/${artifact}-mips"
        "$rsdk/bin/mips-linux-strip" "$output/${artifact}-mips"
        ;;
    *) echo 'Usage: build-openthread-relay.sh [host|mips]' >&2; exit 2 ;;
esac
done
