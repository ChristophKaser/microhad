#!/usr/bin/env bash
# Build the Matter service or synthetic fixtures; no deployment or physical radio access.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$project_root"
mode=${1:-host}
case "$mode" in host|mips) ;; *) exit 2 ;; esac
artifact_kind=${2:-service}
case "$artifact_kind" in
    transport) feature=openthread; binary=ot-matter-transport-probe; output=ot-transport-mips ;;
    commissioning) feature=controller-probe; binary=ot-matter-commissioning-probe; output=ot-commissioning-mips ;;
    service) feature=controller-service; binary=thread-matter; output=thread-matter-mips ;;
    *) exit 2 ;;
esac
bash thread-matter/scripts/build-openthread-transport.sh "$mode"
bash thread-matter/scripts/fetch-rs-matter.sh
source scripts/rust-env.sh
export CARGO_TARGET_DIR="$project_root/build/thread-matter/target"
args=(--locked --release --manifest-path thread-matter/Cargo.toml --features "$feature" --bin "$binary")
if [ "$mode" = mips ]; then
    export RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static --cfg sha2_backend_soft="compact" --cfg aes_backend_soft="compact"'
    args+=(--target targets/lexra-stock-uclibc.json -Z json-target-spec -Z build-std=std,panic_abort)
fi
cargo +$RUST_TOOLCHAIN build "${args[@]}"
if [ "$mode" = mips ]; then
    "$LLVM_STRIP" --keep-section=.gnu.attributes -o "build/thread-matter/$output" \
        "build/thread-matter/target/lexra-stock-uclibc/release/$binary"
    artifact="build/thread-matter/$output"
else
    artifact="build/thread-matter/target/release/$binary"
fi
if [ "$artifact_kind" = service ]; then
    python3 thread-matter/scripts/check-matter-service-test-hooks.py "$artifact" absent
fi
