#!/usr/bin/env bash
# SDK-matched OpenThread POSIX proof build; never contacts the gateway.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mode=${1:-mips}
case "$mode" in host|mips) ;; *) echo 'Usage: build-openthread.sh [host|mips]' >&2; exit 2 ;; esac
sdk="$experiment_root/tools/silabs/gecko_sdk"
expected_sdk=90fc93e1f95a10c981f0e49ca6787192e82fd8f2
[[ $(git -C "$sdk" rev-parse HEAD) == "$expected_sdk" ]] || {
    echo 'Expected the project-pinned GSDK 4.5.0 checkout; run install-silabs.sh.' >&2; exit 1;
}
source_original="$sdk/util/third_party/openthread"
output="$experiment_root/build/openthread-unix"
source_copy="$output/source"
mkdir -p "$output"
if [[ ! -d "$source_copy" ]]; then
    mkdir -p "$source_copy"
    cp -a "$source_original/." "$source_copy/"
fi
patch_file="$experiment_root/patches/openthread-hdlc-unix.patch"
if ! git -C "$source_copy" apply --reverse --check "$patch_file" 2>/dev/null; then
    git -C "$source_copy" apply --check "$patch_file"
    git -C "$source_copy" apply "$patch_file"
fi
python3 - "$experiment_root" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
out = root / 'build/openthread-unix'
rd = root.parent / 'rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129'
cxx = rd / 'include/c++/4.4.7'
# GCC 4.4 exception_ptr.h assumes an earlier type_info declaration.
(out / 'stock-cxx-compat.hpp').write_text('namespace std { class type_info; }\n')
flags = (f'--target=mips-linux-gnu -march=mips1 -msoft-float -mabi=32 '
         f'--sysroot={rd}/mips-linux-uclibc '
         f'-isystem {rd}/lib/gcc/mips-linux-uclibc/4.4.7/include-fixed '
         '-Os -fno-pic -mno-abicalls -G0 -fno-stack-protector -mllvm -mno-check-zero-division '
         '-DOPENTHREAD_POSIX_CONFIG_BACKTRACE_ENABLE=0')
cppflags = (f'{flags} -nostdinc++ -isystem {cxx} -isystem {cxx}/mips-linux-uclibc/4181 '
            f'-fno-exceptions -include {out}/stock-cxx-compat.hpp '
            '-D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS')
(out / 'toolchain.cmake').write_text(f'''set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(CMAKE_C_COMPILER /usr/bin/clang-18)
set(CMAKE_CXX_COMPILER /usr/bin/clang++-18)
set(CMAKE_C_FLAGS_INIT "{flags}")
set(CMAKE_CXX_FLAGS_INIT "{cppflags}")
set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS_INIT}}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${{CMAKE_CXX_FLAGS_INIT}}" CACHE STRING "" FORCE)
set(CMAKE_AR /usr/bin/llvm-ar-18)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_LINK_EXECUTABLE "{rd}/bin/mips-linux-gcc <LINK_FLAGS> <OBJECTS> -o <TARGET> -static -Wl,--gc-sections -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
set(CMAKE_CXX_LINK_EXECUTABLE "{rd}/bin/mips-linux-g++ <LINK_FLAGS> <OBJECTS> -o <TARGET> -static -Wl,--gc-sections -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
''')
cmake = out / 'source/src/posix/CMakeLists.txt'
if 'if(OT_UNIX_ADAPTER_TEST_SOURCE)' not in cmake.read_text():
    with cmake.open('a') as stream:
        stream.write('''
if(OT_UNIX_ADAPTER_TEST_SOURCE)
    add_executable(ot-unix-adapter-test "${OT_UNIX_ADAPTER_TEST_SOURCE}")
    target_include_directories(ot-unix-adapter-test PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/core ${PROJECT_SOURCE_DIR}/src/posix/platform)
    target_link_libraries(ot-unix-adapter-test PRIVATE openthread-posix openthread-ftd openthread-posix)
endif()
''')
PY
options=(-DOT_PLATFORM=posix -DOT_POSIX_RCP_HDLC_BUS=ON -DOT_APP_NCP=OFF
    -DOT_APP_RCP=OFF -DOT_MTD=OFF -DOT_COMPILE_WARNING_AS_ERROR=OFF
    -DOT_SRP_SERVER=ON -DOT_SRP_CLIENT=ON -DOT_DNSSD_SERVER=ON -DOT_DNS_CLIENT=ON -DOT_ECDSA=ON -DOT_SERVICE=ON
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=MinSizeRel)
if [[ "$mode" == mips ]]; then
    options+=(-DCMAKE_TOOLCHAIN_FILE="$output/toolchain.cmake")
else
    options+=(-DOT_UNIX_ADAPTER_TEST_SOURCE="$experiment_root/tests/openthread-unix.cpp")
fi
cmake -S "$source_copy" -B "$output/$mode" "${options[@]}"
cmake --build "$output/$mode" --target ot-cli --parallel "${BUILD_JOBS:-4}"
if [[ "$mode" == host ]]; then
    cmake --build "$output/host" --target ot-unix-adapter-test --parallel "${BUILD_JOBS:-4}"
    "$output/host/src/posix/ot-unix-adapter-test"
else
    llvm-strip-18 --keep-section=.gnu.attributes -o "$output/ot-cli-mips" "$output/mips/src/posix/ot-cli"
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
