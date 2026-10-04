#!/usr/bin/env bash
# Prepare C ABI archive; never contacts a gateway or real radio.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mode=${1:-host}
case "$mode" in host|mips) ;; *) exit 2 ;; esac
bash "$project_root/scripts/build-openthread.sh" "$mode"
source_copy="$project_root/build/openthread-unix/source"
python3 - "$source_copy/src/posix/CMakeLists.txt" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
text = p.read_text().split('\nif(GATEWAY_OT_TRANSPORT_SOURCE)')[0]
p.write_text(text + '''
if(GATEWAY_OT_TRANSPORT_SOURCE)
    add_library(gateway-thread-transport STATIC "${GATEWAY_OT_TRANSPORT_SOURCE}")
    target_compile_options(gateway-thread-transport PRIVATE -ffunction-sections -fdata-sections)
    if(GATEWAY_OT_DISCOVERY_TEST_SOURCE)
        add_library(gateway-thread-discovery-fixture STATIC "${GATEWAY_OT_DISCOVERY_TEST_SOURCE}")
        target_link_libraries(gateway-thread-discovery-fixture PRIVATE openthread-ftd ot-config ot-config-ftd)
        target_compile_options(gateway-thread-discovery-fixture PRIVATE -ffunction-sections -fdata-sections)
    endif()
    target_link_libraries(gateway-thread-transport PUBLIC openthread-posix openthread-ftd PRIVATE ot-config ot-config-ftd)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "mips")
        target_compile_definitions(gateway-thread-transport PRIVATE GATEWAY_OT_FACTORY_UCLIBC=1)
    endif()
endif()
''')
PY
cmake -S "$source_copy" -B "$project_root/build/openthread-unix/$mode" \
    -DGATEWAY_OT_TRANSPORT_SOURCE="$project_root/openthread-transport/adapter.cpp" \
    -DGATEWAY_OT_DISCOVERY_TEST_SOURCE="$project_root/tests/openthread-discovery-fixture.cpp" \
    -DCMAKE_C_FLAGS_RELEASE='-Os -DNDEBUG -ffunction-sections -fdata-sections' \
    -DCMAKE_CXX_FLAGS_RELEASE='-Os -DNDEBUG -ffunction-sections -fdata-sections'
cmake --build "$project_root/build/openthread-unix/$mode" --target gateway-thread-transport gateway-thread-discovery-fixture --parallel "${BUILD_JOBS:-4}"
