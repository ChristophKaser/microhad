#!/usr/bin/env bash
# Official Silicon Labs release, pinned 1.16.4; installs locally, no root/probe.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
archive="$experiment_root/tools/silabs/commander-download/commander.tar.zst"
expected=0d1a2136a824c2f3e9953573502cb80ff459647f41df04d17a30e42140432f8a
mkdir -p "$(dirname "$archive")" "$experiment_root/tools/silabs/commander"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --output "$archive.new" https://github.com/SiliconLabs/arduino/releases/download/2.0.0/Commander_linux_x86_64_1v16p4b1564.tar.zst
    [[ $(sha256sum "$archive.new" | cut -d' ' -f1) == "$expected" ]] || { echo 'Download digest mismatch' >&2;exit 1; }
    mv "$archive.new" "$archive"
fi
[[ $(sha256sum "$archive" | cut -d' ' -f1) == "$expected" ]] || { echo 'Commander digest mismatch' >&2;exit 1; }
tar --warning=no-unknown-keyword --zstd -xf "$archive" -C "$experiment_root/tools/silabs/commander"
"$experiment_root/tools/silabs/commander/commander/commander" --version
