#!/usr/bin/env bash
# Resolve the same settings used by Make and Python. Host builds need no SDK.
toolchain_script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ZIGGURAT_ROOT=$(cd -- "$toolchain_script_dir/.." && pwd)
toolchain_assignments=$(python3 "$toolchain_script_dir/toolchain_config.py" --shell) || return
eval "$toolchain_assignments"
export ZIGGURAT_ROOT
