#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
cd "$experiment_root"
mkdir -p build patches
cargo fetch --manifest-path dependency-probe/Cargo.toml
python3 scripts/prepare-crate-patches.py getrandom-0.4.3 nix-0.26.4
probe_patches=(
    --config "patch.crates-io.getrandom.path=\"$experiment_root/build/patched-crates/getrandom-0.4.3\""
    --config "patch.crates-io.nix.path=\"$experiment_root/build/patched-crates/nix-0.26.4\""
)
case "${1:-host}" in
    host) cargo run --release --manifest-path dependency-probe/Cargo.toml "${probe_patches[@]}" ;;
    stock)
        bash scripts/make-llvm-target.sh
        RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static' \
            cargo build --release --manifest-path dependency-probe/Cargo.toml \
            --target targets/lexra-stock-uclibc.json \
            -Z json-target-spec -Z build-std=std,panic_abort "${probe_patches[@]}"
        ;;
    *) echo "Usage: bash scripts/dependencies.sh [host|stock]" >&2; exit 2 ;;
esac
