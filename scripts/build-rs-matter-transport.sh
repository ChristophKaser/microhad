#!/usr/bin/env bash
# Build the isolated transport probe; no deployment or physical radio access.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"
mode=${1:-host}
case "$mode" in host|mips) ;; *) exit 2 ;; esac
bash scripts/build-openthread-transport.sh "$mode"
source scripts/rust-env.sh
export CARGO_TARGET_DIR="$project_root/build/rs-matter/target"
args=(--locked --release --manifest-path rs-matter-probe/Cargo.toml --features openthread --bin ot-matter-transport-probe)
if [ "$mode" = mips ]; then
    export RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static --cfg sha2_backend_soft="compact" --cfg aes_backend_soft="compact"'
    args+=(--target targets/lexra-stock-uclibc.json -Z json-target-spec -Z build-std=std,panic_abort)
fi
cargo +nightly-2026-10-02 build "${args[@]}"
if [ "$mode" = mips ]; then
    llvm-strip-18 --keep-section=.gnu.attributes -o build/rs-matter/ot-transport-mips \
        build/rs-matter/target/lexra-stock-uclibc/release/ot-matter-transport-probe
fi
