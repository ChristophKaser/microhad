#!/usr/bin/env bash
# Prepare C ABI archive; never contacts a gateway or real radio.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$project_root"
mode=${1:-host}
case "$mode" in host|mips) ;; *) exit 2 ;; esac
bash "$project_root/micro-thread-matter-daemon/scripts/build-openthread.sh" "$mode"
source "$project_root/scripts/toolchains.sh"
source_copy="$project_root/build/openthread-unix/source-mailbox"
output="$project_root/build/openthread-unix"
if [[ "$mode" == mips ]]; then output="$project_root/build/${TARGET_BUILD_SUBDIR}openthread-unix"; fi
cmake -S "$source_copy" -B "$output/$mode" \
    -DGATEWAY_OT_TRANSPORT_SOURCE="$project_root/micro-thread-matter-daemon/openthread-transport/adapter.cpp" \
    -DGATEWAY_OT_DISCOVERY_TEST_SOURCE="$project_root/micro-thread-matter-daemon/tests/openthread-discovery-fixture.cpp" \
    -DCMAKE_C_FLAGS_RELEASE='-Os -DNDEBUG -ffunction-sections -fdata-sections' \
    -DCMAKE_CXX_FLAGS_RELEASE='-Os -DNDEBUG -ffunction-sections -fdata-sections'
cmake --build "$output/$mode" --target gateway-thread-transport gateway-thread-discovery-fixture --parallel "${BUILD_JOBS:-4}"
