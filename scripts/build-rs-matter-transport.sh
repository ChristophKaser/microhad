#!/usr/bin/env bash
# Build the isolated transport probe; no deployment or physical radio access.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"
mode=${1:-host}
case "$mode" in host|mips) ;; *) exit 2 ;; esac
probe=${2:-transport}
case "$probe" in
    transport) feature=openthread; binary=ot-matter-transport-probe; output=ot-transport-mips ;;
    commissioning) feature=controller-probe; binary=ot-matter-commissioning-probe; output=ot-commissioning-mips ;;
    service) feature=controller-service; binary=thread-matter; output=thread-matter-mips ;;
    *) exit 2 ;;
esac
bash scripts/build-openthread-transport.sh "$mode"
source scripts/rust-env.sh
export CARGO_TARGET_DIR="$project_root/build/rs-matter/target"
args=(--locked --release --manifest-path rs-matter-probe/Cargo.toml --features "$feature" --bin "$binary")
if [ "$mode" = mips ]; then
    export RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static --cfg sha2_backend_soft="compact" --cfg aes_backend_soft="compact"'
    args+=(--target targets/lexra-stock-uclibc.json -Z json-target-spec -Z build-std=std,panic_abort)
fi
cargo +nightly-2026-10-02 build "${args[@]}"
if [ "$mode" = mips ]; then
    llvm-strip-18 --keep-section=.gnu.attributes -o "build/rs-matter/$output" \
        "build/rs-matter/target/lexra-stock-uclibc/release/$binary"
    artifact="build/rs-matter/$output"
else
    artifact="build/rs-matter/target/release/$binary"
fi
if [ "$probe" = service ]; then
    python3 scripts/check-matter-service-test-hooks.py "$artifact" absent
fi
