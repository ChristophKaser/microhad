#!/bin/sh
# Build BusyBox 1.13.4's logread applet against the gateway toolchain.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rsdk=${RSDK_ROOT:-"$project_dir/../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129"}
compiler_prefix="$rsdk/bin/mips-linux-"
build_dir="$project_dir/build/busybox-1.13.4"
archive="$project_dir/build/busybox-1.13.4.tar.bz2"
archive_sha256=43ff187937f0bf38b23a926f4215be021c9f937b497912d785a91fbdf180b94b
source_dir="$build_dir/source"
mkdir -p "$build_dir"
if [ ! -f "$archive" ]; then
    curl -fL https://busybox.net/downloads/busybox-1.13.4.tar.bz2 -o "$archive"
fi
actual_sha256=$(sha256sum "$archive" | awk '{print $1}')
[ "$actual_sha256" = "$archive_sha256" ] || { echo "BusyBox source checksum mismatch" >&2; exit 1; }
if [ ! -f "$source_dir/Makefile" ]; then
    mkdir -p "$source_dir"
    tar -xjf "$archive" -C "$source_dir" --strip-components=1
fi
python3 - "$source_dir/Makefile" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("config %config:", "config:\n%config:")
text = text.replace("/ %/: prepare scripts FORCE", "/: prepare scripts FORCE\n%/: prepare scripts FORCE")
path.write_text(text)
PY
make -s -C "$source_dir" scripts_basic
make -s -C "$source_dir" scripts/kconfig/conf
if [ ! -f "$source_dir/.config" ]; then
    (cd "$source_dir" && yes "" | scripts/kconfig/conf -n Config.in) >/dev/null 2>&1 || true
fi
python3 - "$source_dir/.config" "$compiler_prefix" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
values = {
    "CONFIG_STATIC": "y",
    "CONFIG_CROSS_COMPILER_PREFIX": sys.argv[2],
    "CONFIG_SYSLOGD": "y",
    "CONFIG_FEATURE_REMOTE_LOG": "y",
    "CONFIG_FEATURE_IPC_SYSLOG": "y",
    "CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE": "512",
    "CONFIG_LOGREAD": "y",
}
lines = path.read_text().splitlines()
for key, value in values.items():
    replacement = f"{key}={value}"
    for index, line in enumerate(lines):
        if line.startswith(key + "=") or line == f"# {key} is not set":
            lines[index] = replacement
            break
    else:
        lines.append(replacement)
path.write_text("\n".join(lines) + "\n")
PY
(cd "$source_dir" && yes "" | scripts/kconfig/conf -o Config.in) >/dev/null 2>&1 || true
make -s -C "$source_dir" CROSS_COMPILE="$compiler_prefix" ARCH=mips busybox
output="$project_dir/build/logread-mips"
"${compiler_prefix}strip" -o "$output" "$source_dir/busybox"
"${compiler_prefix}readelf" -h "$output" | grep -q 'Data:.*big endian'
"${compiler_prefix}readelf" -h "$output" | grep -q 'Machine:.*MIPS'
echo "Built $output"
