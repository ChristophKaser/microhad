#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
cd "$experiment_root"
mkdir -p logs

case "${1:-host}" in
    host)
        cargo run --release --manifest-path prototype/Cargo.toml
        ;;
    codegen)
        bash scripts/make-llvm-target.sh
        cargo rustc --manifest-path prototype/Cargo.toml --lib --release \
            --target targets/lexra-mips1.json -Z json-target-spec \
            -Z build-std=core -- --emit=asm
        ;;
    stock|stock-compat)
        bash scripts/make-llvm-target.sh
        prototype_features=()
        if [[ "$1" == stock-compat ]]; then
            prototype_features=(--features stock-compat)
        fi
        RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static' \
            cargo build --manifest-path prototype/Cargo.toml --bin lexra-rust-prototype \
            --release --target targets/lexra-stock-uclibc.json \
            -Z json-target-spec -Z build-std=std,panic_abort "${prototype_features[@]}"
        ;;
    *)
        echo "Usage: bash scripts/prototype.sh [host|codegen|stock|stock-compat]" >&2
        exit 2
        ;;
esac
