#!/usr/bin/env bash
# Build the pinned host commissioner with opt-in, normally negotiated key export.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
source_dir=${CHIP_SOURCE_DIR:-"$root/build/connectedhomeip/source"}
output_dir=${CHIP_BUILD_DIR:-"$root/build/out/linux-x64-chip-tool-clang-release"}
pin=43c0a7d29f8d251d3eef0a7333d3b68e421eed81
diagnostic_dir="$root/scripts/matter-diagnostics"
[[ $(git -C "$source_dir" rev-parse HEAD) == "$pin" ]] || {
    echo "Expected connectedhomeip $pin in $source_dir" >&2; exit 1;
}
if git -C "$source_dir" apply --reverse --check "$diagnostic_dir/controller-key-export.patch" 2>/dev/null; then
    : # Already installed; do not discard unrelated local SDK changes.
else
    git -C "$source_dir" apply --check "$diagnostic_dir/controller-key-export.patch"
    git -C "$source_dir" apply "$diagnostic_dir/controller-key-export.patch"
fi
if ! git -C "$source_dir" apply --reverse --check "$diagnostic_dir/split-logger.patch" 2>/dev/null; then
    git -C "$source_dir" apply --check "$diagnostic_dir/split-logger.patch"
    git -C "$source_dir" apply "$diagnostic_dir/split-logger.patch"
fi
python3 "$diagnostic_dir/split-logger.py" "$source_dir"
cmp -s "$diagnostic_dir/DiagnosticKeyExport.h" "$source_dir/src/transport/DiagnosticKeyExport.h" || \
    cp "$diagnostic_dir/DiagnosticKeyExport.h" "$source_dir/src/transport/DiagnosticKeyExport.h"
[[ -f "$source_dir/.environment/build_overrides/pigweed_environment.gni" ]] || {
    echo 'Bootstrap the existing CHIP SDK build environment first.' >&2; exit 1;
}
# Activating Pigweed runs online CIPD checks. Its existing environment is sufficient.
export _PW_ACTUAL_ENVIRONMENT_ROOT="$source_dir/.environment"
export PATH="$source_dir/.environment/pigweed-venv/bin:$source_dir/.environment/cipd/packages/zap:$source_dir/.environment/cipd/packages/pigweed/bin:$PATH"
export ZAP_TEMPSTATE=1
if [[ ! -f "$output_dir/build.ninja" ]]; then
    mkdir -p "$output_dir"
    "$source_dir/.environment/cipd/packages/pigweed/gn" --root="$source_dir" gen "$output_dir" \
        --args='is_debug=false optimize_for_size=false is_clang=true chip_support_thread_meshcop=false chip_enable_transport_trace=true'
fi
ninja -C "$output_dir" -j "${CHIP_BUILD_JOBS:-3}" chip-tool
if [[ ${WITH_FIXTURE:-0} == 1 ]]; then
    ninja -C "$output_dir" -j "${CHIP_BUILD_JOBS:-3}" examples/all-clusters-app/linux:chip-all-clusters-app
fi
destination="$root/build/matter-diagnostics"
mkdir -p "$destination"
cp "$output_dir/chip-tool" "$destination/chip-tool"
if [[ ${WITH_FIXTURE:-0} == 1 ]]; then
    cp "$output_dir/chip-all-clusters-app" "$destination/chip-all-clusters-app"
fi
python3 - "$destination" "$pin" "$diagnostic_dir" <<'PY'
import hashlib, json, pathlib, sys
dest, pin, source = pathlib.Path(sys.argv[1]), sys.argv[2], pathlib.Path(sys.argv[3])
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
manifest = {'schema': 1, 'source_revision': pin, 'exporter_schema': 1,
            'binary_sha256': digest(dest / 'chip-tool'),
            'patch_sha256': digest(source / 'controller-key-export.patch'),
            'header_sha256': digest(source / 'DiagnosticKeyExport.h'),
            'logger_split_sha256': digest(source / 'split-logger.py'),
            'logger_patch_sha256': digest(source / 'split-logger.patch'),
            'security_test_mode': False}
(dest / 'controller-build.json').write_text(json.dumps(manifest, indent=2) + '\n')
# A changed build must pass the fixture rehearsal again.
(dest / 'rehearsal.json').unlink(missing_ok=True)
print(dest / 'chip-tool')
PY
