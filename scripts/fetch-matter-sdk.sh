#!/usr/bin/env bash
# Fetch only the pinned files needed for the Matter compiler feasibility gate.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$project_root/build/matter-sdk/source"
revision=3bcdd56ba54fb88b2afb4bfef575014671df7aa7
if [[ ! -e "$source_dir" ]]; then
    git clone --depth 1 --filter=blob:none --sparse --branch v1.6.1.0 \
        https://github.com/project-chip/connectedhomeip.git "$source_dir"
fi
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]] || {
    echo "Expected Matter SDK v1.6.1.0 at $revision" >&2
    exit 1
}
[[ -z $(git -C "$source_dir" status --porcelain) ]] || {
    echo 'Matter SDK checkout must be unmodified' >&2
    exit 1
}
git -C "$source_dir" sparse-checkout set src/lib/support src/controller src/inet \
    src/ble src/crypto src/platform/OpenThread build/config examples/chip-tool
echo "Matter SDK compiler-probe source: $source_dir ($revision)"
