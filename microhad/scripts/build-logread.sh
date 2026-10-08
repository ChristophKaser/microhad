#!/usr/bin/env bash
# Build a logread-only BusyBox 1.13.4 binary against the gateway toolchain.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source "$project_dir/../scripts/toolchains.sh"
compiler_prefix="$RSDK_ROOT/bin/$TARGET_PREFIX"
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
python3 - "$source_dir/sysklogd/Config.in" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
block = '''config FEATURE_IPC_SYSLOG
\tbool "Circular Buffer support"
\tdefault n
\tdepends on SYSLOGD
'''
replacement = '''config FEATURE_IPC_SYSLOG
\tbool "Circular Buffer support"
\tdefault n
'''
if block in text:
    path.write_text(text.replace(block, replacement, 1))
elif replacement not in text:
    raise SystemExit("BusyBox FEATURE_IPC_SYSLOG Kconfig block changed")
PY
python3 - "$source_dir/Makefile" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("config %config:", "config:\n%config:")
text = text.replace("/ %/: prepare scripts FORCE", "/: prepare scripts FORCE\n%/: prepare scripts FORCE")
path.write_text(text)
PY
if [ ! -f "$source_dir/.config" ]; then
    make -s -C "$source_dir" allnoconfig
fi
python3 - "$source_dir/.config" "$compiler_prefix" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
values = {
    "CONFIG_STATIC": "y",
    "CONFIG_CROSS_COMPILER_PREFIX": sys.argv[2],
    "CONFIG_SYSLOGD": None,
    "CONFIG_FEATURE_REMOTE_LOG": None,
    "CONFIG_FEATURE_IPC_SYSLOG": "y",
    "CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE": "512",
    "CONFIG_LOGREAD": "y",
}
lines = path.read_text().splitlines()
for key, value in values.items():
    replacement = f"{key}={value}" if value is not None else f"# {key} is not set"
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
