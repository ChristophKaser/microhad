#!/usr/bin/env bash
# SDK-matched OpenThread POSIX proof build; never contacts the gateway.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$experiment_root"
source "$experiment_root/scripts/toolchains.sh"
mode=${1:-mips}
case "$mode" in host|mips) ;; *) echo 'Usage: build-openthread.sh [host|mips]' >&2; exit 2 ;; esac
sdk="$experiment_root/shared/dependencies/gecko-sdk"
expected_sdk=90fc93e1f95a10c981f0e49ca6787192e82fd8f2
[[ $(git -C "$sdk" rev-parse HEAD) == "$expected_sdk" ]] || {
    echo 'Expected the project-pinned GSDK 4.5.0 checkout; run install-silabs.sh.' >&2; exit 1;
}
source_original="$sdk/util/third_party/openthread"
output="$experiment_root/build/openthread-unix"
source_copy="$output/source-mailbox"
mkdir -p "$output"
if [[ ! -d "$source_copy" ]]; then
    cp -a "$source_original/." "$source_copy/"
    git -C "$source_copy" init --quiet -b mailbox-base
    git -C "$source_copy" config user.name 'Pinned Gecko SDK source'
    git -C "$source_copy" config user.email 'gecko-source@localhost'
    git -C "$source_copy" add -A
    git -C "$source_copy" commit --quiet -m 'Import pinned Gecko SDK OpenThread source'
    git -C "$source_copy" tag mailbox-base
fi
base_revision=$(git -C "$source_copy" rev-parse refs/tags/mailbox-base)
python3 scripts/apply-mailbox-series.py "$source_copy" "$base_revision" \
    "$experiment_root/thread-matter/patches/openthread-hdlc-unix.patch" \
    "$experiment_root/thread-matter/patches/openthread-hdlc-test.patch" \
    "$experiment_root/thread-matter/patches/openthread-thread-transport.patch"
python3 - "$experiment_root" <<'PY'
import os
from pathlib import Path
import sys
root = Path(sys.argv[1])
out = root / 'build/openthread-unix'
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
set(CMAKE_C_LINK_EXECUTABLE "{os.environ['TARGET_CC']} <LINK_FLAGS> <OBJECTS> -o <TARGET> -static -Wl,--gc-sections -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
set(CMAKE_CXX_LINK_EXECUTABLE "{os.environ['TARGET_CXX']} <LINK_FLAGS> <OBJECTS> -o <TARGET> -static -Wl,--gc-sections -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
''')
PY
options=(-DOT_PLATFORM=posix -DOT_POSIX_RCP_HDLC_BUS=ON -DOT_APP_NCP=OFF
    -DOT_APP_RCP=OFF -DOT_MTD=OFF -DOT_COMPILE_WARNING_AS_ERROR=OFF
    -DOT_SRP_SERVER=ON -DOT_SRP_CLIENT=ON -DOT_DNSSD_SERVER=ON -DOT_DNS_CLIENT=ON -DOT_ECDSA=ON -DOT_SERVICE=ON
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=MinSizeRel)
if [[ "$mode" == mips ]]; then
    options+=(-DCMAKE_TOOLCHAIN_FILE="$output/toolchain.cmake")
else
    options+=(-DOT_UNIX_ADAPTER_TEST_SOURCE="$experiment_root/thread-matter/tests/openthread-unix.cpp")
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
    python3 - "$output/ot-cli-mips" <<'PYVERIFY'
import subprocess
import sys
binary = sys.argv[1]
header = subprocess.check_output(['readelf', '-h', binary], text=True)
attributes = subprocess.check_output(['readelf', '-A', binary], text=True)
segments = subprocess.check_output(['readelf', '-l', binary], text=True)
dynamic = subprocess.check_output(['readelf', '-d', binary], text=True)
assert 'ELF32' in header and 'big endian' in header and 'MIPS' in header
assert 'mips1' in header and 'o32' in header and 'Soft float' in attributes
assert 'INTERP' not in segments and 'There is no dynamic section' in dynamic
print('Target verification passed: big-endian MIPS-I/o32, soft float, static')
PYVERIFY
    file "$output/ot-cli-mips"
    readelf -h "$output/ot-cli-mips"
    readelf -A "$output/ot-cli-mips" | sed -n '1,5p'
    sha256sum "$output/ot-cli-mips"
fi
