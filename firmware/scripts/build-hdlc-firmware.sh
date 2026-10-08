#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/silabs-env.sh"
project=ot-rcp-multipan
build_dir="$experiment_root/build/firmware-hdlc"
patches_dir="$experiment_root/firmware/dependencies/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/patches"
revision=90fc93e1f95a10c981f0e49ca6787192e82fd8f2
[[ $(git -C "$GECKO_SDK" rev-parse HEAD) == "$revision" ]] || { echo 'Wrong Gecko SDK revision' >&2; exit 1; }
command -v slc >/dev/null
command -v arm-none-eabi-gcc >/dev/null
mkdir -p "$build_dir" "$experiment_root/logs"
cp "$experiment_root/firmware/hdlc-multipan/$project.slcp" "$build_dir/"
cp "$patches_dir/main.c" "$build_dir/"
cp "$GECKO_SDK/protocol/openthread/sample-apps/ot-ncp/"{app.c,app.h} "$build_dir/"
cp "$GECKO_SDK/util/third_party/openthread/src/lib/platform/reset_util.h" "$build_dir/"
cd "$build_dir"
slc signature trust --sdk "$GECKO_SDK"
slc generate "$project.slcp" --sdk "$GECKO_SDK" --with EFR32MG1B232F256GM48 --force \
    > "$experiment_root/logs/hdlc-firmware-generate.log" 2>&1
cp "$patches_dir/sl_uartdrv_usart_vcom_config.h" config/
cp "$patches_dir/sl_rail_util_pti_config.h" config/
python3 - "$project.Makefile" "$ARM_GCC_DIR" <<'PY'
import pathlib,re,sys
p=pathlib.Path(sys.argv[1]);s=p.read_text()
s=re.sub(r'^ARM_GCC_DIR_LINUX\s*=.*', 'ARM_GCC_DIR_LINUX = '+sys.argv[2],s,flags=re.M)
marker='-include ot-rcp-multipan.project.mak'
extra='''
C_FLAGS := $(subst -Os,-Oz,$(C_FLAGS))
CXX_FLAGS := $(subst -Os,-Oz,$(CXX_FLAGS))
C_FLAGS := $(subst -Werror=unused-label,,$(C_FLAGS))
C_DEFS += '-DOPENTHREAD_CONFIG_PLATFORM_BOOTLOADER_MODE_ENABLE=1'
'''
if '# HDLC experiment flags' not in s:s=s.replace(marker,marker+'\n# HDLC experiment flags\n'+extra)
p.write_text(s)
PY
make -f "$project.Makefile" -j"${BUILD_JOBS:-4}" \
    > "$experiment_root/logs/hdlc-firmware-build.log" 2>&1
arm-none-eabi-size "build/debug/$project.out"
arm-none-eabi-nm "build/debug/$project.out" | rg 'otAppNcpInitMulti|otNcpHdlcInitMulti'
arm-none-eabi-objcopy -O binary "build/debug/$project.out" "build/debug/$project.bin"
echo "Built firmware under $build_dir; no device was accessed."
