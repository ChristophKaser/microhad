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
    service) feature=controller-service; binary=mthread-matter; output=mthread-matter-mips ;;
    *) exit 2 ;;
esac
bash micro-thread-matter-daemon/scripts/build-openthread-transport.sh "$mode"
bash micro-thread-matter-daemon/scripts/fetch-rs-matter.sh
source scripts/rust-env.sh
output_root="$project_root/build/micro-thread-matter-daemon"
if [ "$mode" = mips ]; then
    output_root="$project_root/build/${TARGET_BUILD_SUBDIR}micro-thread-matter-daemon"
    python3 scripts/toolchain-build.py "$output_root/target/$TARGET_SPEC_NAME"
    bash scripts/make-llvm-target.sh
fi
export CARGO_TARGET_DIR="$output_root/target"
args=(--locked --release --manifest-path micro-thread-matter-daemon/Cargo.toml --features "$feature" --bin "$binary")
if [ "$mode" = mips ]; then
    export RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static --cfg sha2_backend_soft="compact" --cfg aes_backend_soft="compact"'
    if [[ "$LINK_MODE" == dynamic ]]; then
        RUSTFLAGS=${RUSTFLAGS/-C target-feature=+crt-static -C relocation-model=static/-C target-feature=-crt-static -C relocation-model=pic}
    fi
    args+=(--target "targets/$TARGET_SPEC_NAME.json" -Z json-target-spec -Z build-std=std,panic_abort)
fi
cargo +$RUST_TOOLCHAIN build "${args[@]}"
if [ "$mode" = mips ]; then
    "$LLVM_STRIP" --keep-section=.gnu.attributes -o "$output_root/$output" \
        "$CARGO_TARGET_DIR/$TARGET_SPEC_NAME/release/$binary"
    artifact="$output_root/$output"
    python3 scripts/verify-gateway-target.py --link-mode "$LINK_MODE" "$artifact"
else
    artifact="build/micro-thread-matter-daemon/target/release/$binary"
fi
if [ "$artifact_kind" = service ]; then
    python3 micro-thread-matter-daemon/scripts/check-matter-service-test-hooks.py "$artifact" absent
fi
