#!/usr/bin/env bash
# Prepare C ABI archive; never contacts a gateway or real radio.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$project_root"
mode=${1:-host}
case "$mode" in host|mips) ;; *) exit 2 ;; esac
bash "$project_root/micro-thread-matter-daemon/scripts/build-openthread.sh" "$mode"
source_copy="$project_root/build/openthread-unix/source-mailbox"
cmake -S "$source_copy" -B "$project_root/build/openthread-unix/$mode" \
    -DGATEWAY_OT_TRANSPORT_SOURCE="$project_root/micro-thread-matter-daemon/openthread-transport/adapter.cpp" \
    -DGATEWAY_OT_DISCOVERY_TEST_SOURCE="$project_root/micro-thread-matter-daemon/tests/openthread-discovery-fixture.cpp" \
    -DCMAKE_C_FLAGS_RELEASE='-Os -DNDEBUG -ffunction-sections -fdata-sections' \
    -DCMAKE_CXX_FLAGS_RELEASE='-Os -DNDEBUG -ffunction-sections -fdata-sections'
cmake --build "$project_root/build/openthread-unix/$mode" --target gateway-thread-transport gateway-thread-discovery-fixture --parallel "${BUILD_JOBS:-4}"
