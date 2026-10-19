#!/usr/bin/env bash
# Pinned upstream OpenThread POSIX build; never contacts the gateway.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$experiment_root"
source "$experiment_root/scripts/toolchains.sh"
mode=${1:-mips}
case "$mode" in host|mips) ;; *) echo 'Usage: build-openthread.sh [host|mips]' >&2; exit 2 ;; esac
output="$experiment_root/build/openthread-unix"
source_copy="$output/source-mailbox"
if [[ "$mode" == mips ]]; then
    output="$experiment_root/build/${TARGET_BUILD_SUBDIR}openthread-unix"
    python3 scripts/toolchain-build.py "$output/mips"
fi
python3 micro-thread-matter-daemon/scripts/prepare-openthread.py
if [[ "$mode" == mips ]]; then
python3 - "$experiment_root" "$output" <<'PY'
import os
from pathlib import Path
import sys
root = Path(sys.argv[1])
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
if os.environ['TOOLCHAIN'] == 'crosstool-ng':
    import subprocess
    cc, cxx = os.environ['TARGET_CC'], os.environ['TARGET_CXX']
    sysroot = subprocess.check_output([cc, '-print-sysroot'], text=True).strip()
    flags = f'-D_DEFAULT_SOURCE -Os -march=lx4380 -msoft-float -mabi=32 -fno-pic -mabicalls -G0 -fno-stack-protector -DOPENTHREAD_POSIX_CONFIG_BACKTRACE_ENABLE=0'
    (out / 'toolchain.cmake').write_text(f'''set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(CMAKE_C_COMPILER {cc})
set(CMAKE_CXX_COMPILER {cxx})
set(CMAKE_SYSROOT {sysroot})
set(CMAKE_AR {os.environ['TARGET_AR']})
set(CMAKE_C_FLAGS_INIT "{flags}")
set(CMAKE_CXX_FLAGS_INIT "{flags} -fno-exceptions")
set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS_INIT}}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${{CMAKE_CXX_FLAGS_INIT}}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -Wl,--gc-sections")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
''')
    raise SystemExit(0)
link_flags = ('-L' + os.environ['STOCK_ROOTFS'] + '/link') if os.environ['LINK_MODE'] == 'dynamic' else '-static'
rd = Path(os.environ['RSDK_ROOT'])
cxx = rd / 'include/c++/4.4.7'
# GCC 4.4 exception_ptr.h assumes an earlier type_info declaration.
(out / 'stock-cxx-compat.hpp').write_text('namespace std { class type_info; }\n')
flags = (f'--target=mips-linux-gnu -march=mips1 -msoft-float -mabi=32 '
         f'--sysroot={rd}/mips-linux-uclibc '
         f'-isystem {rd}/lib/gcc/mips-linux-uclibc/4.4.7/include-fixed '
         '-Os -fno-pic -mabicalls -G0 -fno-stack-protector -mllvm -mno-check-zero-division '
         '-DOPENTHREAD_POSIX_CONFIG_BACKTRACE_ENABLE=0')
cppflags = (f'{flags} -nostdinc++ -isystem {cxx} -isystem {cxx}/mips-linux-uclibc/4181 '
            f'-fno-exceptions -include {out}/stock-cxx-compat.hpp '
            '-D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS')
(out / 'toolchain.cmake').write_text(f'''set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(CMAKE_C_COMPILER {os.environ['CLANG']})
set(CMAKE_CXX_COMPILER {os.environ['CLANGXX']})
set(CMAKE_C_FLAGS_INIT "{flags}")
set(CMAKE_CXX_FLAGS_INIT "{cppflags}")
set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS_INIT}}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${{CMAKE_CXX_FLAGS_INIT}}" CACHE STRING "" FORCE)
set(CMAKE_AR {os.environ['LLVM_AR']})
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_LINK_EXECUTABLE "{os.environ['TARGET_CC']} <LINK_FLAGS> <OBJECTS> -o <TARGET> {link_flags} -Wl,--gc-sections -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
set(CMAKE_CXX_LINK_EXECUTABLE "{os.environ['TARGET_CXX']} <LINK_FLAGS> <OBJECTS> -o <TARGET> {link_flags} -Wl,--gc-sections -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
''')
PY
fi
options=(-DGATEWAY_OT_TRANSPORT_SOURCE="$experiment_root/micro-thread-matter-daemon/openthread-transport/adapter.cpp"
    -DGATEWAY_OT_DISCOVERY_TEST_SOURCE="$experiment_root/micro-thread-matter-daemon/tests/openthread-discovery-fixture.cpp"
    -DOT_PLATFORM=posix -DOT_POSIX_RCP_HDLC_BUS=ON -DOT_APP_NCP=OFF
    -DOT_APP_RCP=OFF -DOT_MTD=OFF -DOT_COMPILE_WARNING_AS_ERROR=OFF
    -DOT_LOG_LEVEL=INFO -DOT_LOG_OUTPUT=PLATFORM_DEFINED
    -DOT_BORDER_ROUTER=ON -DOT_BORDER_ROUTING=OFF
    -DOT_SRP_SERVER=ON -DOT_SRP_CLIENT=ON -DOT_DNSSD_SERVER=ON -DOT_DNS_CLIENT=ON -DOT_ECDSA=ON -DOT_SERVICE=ON
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=MinSizeRel)
if [[ "$mode" == mips ]]; then
    options+=(-DCMAKE_TOOLCHAIN_FILE="$output/toolchain.cmake"
        "-DOT_MBEDTLS_CONFIG_FILE=\"$experiment_root/micro-thread-matter-daemon/openthread-transport/mbedtls-mips-config.h\"")
else
    options+=(-DOT_UNIX_ADAPTER_TEST_SOURCE="$experiment_root/micro-thread-matter-daemon/tests/openthread-unix.cpp")
fi
build_dir="$output/$mode"
if [[ -f "$build_dir/CMakeCache.txt" ]] && ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$source_copy" "$build_dir/CMakeCache.txt"; then
    rm -rf "$build_dir"
fi
cmake -S "$source_copy" -B "$build_dir" "${options[@]}"
cmake --build "$build_dir" --target ot-cli --parallel "${BUILD_JOBS:-4}"
if [[ "$mode" == host ]]; then
    cmake --build "$build_dir" --target ot-unix-adapter-test --parallel "${BUILD_JOBS:-4}"
    "$build_dir/src/posix/ot-unix-adapter-test"
else
    "$LLVM_STRIP" --keep-section=.gnu.attributes -o "$output/ot-cli-mips" "$build_dir/src/posix/ot-cli"
    python3 scripts/verify-gateway-target.py --link-mode "$LINK_MODE" "$output/ot-cli-mips"
    file "$output/ot-cli-mips"
    readelf -h "$output/ot-cli-mips"
    readelf -A "$output/ot-cli-mips" | sed -n '1,5p'
    sha256sum "$output/ot-cli-mips"
fi
