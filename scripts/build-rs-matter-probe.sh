#!/usr/bin/env bash
# Compile an isolated feasibility probe; does not deploy or enable Matter.
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
mode=${1:-host}
case "$mode" in host|mips) ;; *) echo 'Usage: build-rs-matter-probe.sh host|mips' >&2; exit 2 ;; esac
bash "$experiment_root/scripts/fetch-rs-matter.sh"
export CARGO_TARGET_DIR="$experiment_root/build/rs-matter/target"
args=(+nightly-2026-10-02 build --locked --release --manifest-path "$experiment_root/rs-matter-probe/Cargo.toml")
if [[ "$mode" == mips ]]; then
    bash "$experiment_root/scripts/make-llvm-target.sh"
    export RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static --cfg sha2_backend_soft="compact" --cfg aes_backend_soft="compact"'
    args+=(--target "$experiment_root/targets/lexra-stock-uclibc.json" -Z json-target-spec -Z build-std=std,panic_abort)
else
    unset RUSTFLAGS
fi
cargo "${args[@]}"
