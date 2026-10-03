#!/usr/bin/env bash
# Offline packaging only. Commander download is pinned; no probe/device commands.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
archive="$experiment_root/tools/silabs/commander-download/commander.tar.zst"
commander="$experiment_root/tools/silabs/commander/commander/commander"
expected=0d1a2136a824c2f3e9953573502cb80ff459647f41df04d17a30e42140432f8a
[[ $(sha256sum "$archive" | cut -d' ' -f1) == "$expected" ]] || { echo 'Commander archive digest mismatch' >&2; exit 1; }
[[ $(sha256sum "$commander" | cut -d' ' -f1) == eb77fd77d81ab36fb89750c4e3d252ef573c9cad5e9ccce8e4cb505f39c7d996 ]] || { echo 'Commander executable digest mismatch' >&2;exit 1; }
"$commander" --version | rg '1v16p4b1564'
output="$experiment_root/build/firmware-hdlc/build/debug/ot-rcp-multipan.gbl"
"$commander" gbl create "$output" --app "$experiment_root/build/firmware-hdlc/build/debug/ot-rcp-multipan.out"
"$experiment_root/tools/network-migration-venv/bin/python" "$experiment_root/../microhad-zigbee-native/scripts/ezsp_preflight.py" image "$output"
sha256sum "$output"
