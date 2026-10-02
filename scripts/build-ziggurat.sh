#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
cd "$experiment_root"
mkdir -p worktrees build logs
source_revision=$(git -C ziggurat rev-parse HEAD)
if [[ ! -d worktrees/ziggurat ]]; then
    git -C ziggurat worktree add --detach ../worktrees/ziggurat "$source_revision"
fi
if [[ $(git -C worktrees/ziggurat rev-parse HEAD) != "$source_revision" ]]; then
    echo "Build worktree does not match the pinned Ziggurat revision" >&2
    exit 1
fi
if ! git -C worktrees/ziggurat apply --reverse --check \
    "$experiment_root/patches/ziggurat-stock-compat.patch" 2>/dev/null; then
    git -C worktrees/ziggurat apply --check "$experiment_root/patches/ziggurat-stock-compat.patch"
    git -C worktrees/ziggurat apply "$experiment_root/patches/ziggurat-stock-compat.patch"
fi
bash scripts/make-llvm-target.sh
export CARGO_TARGET_DIR="$experiment_root/build/ziggurat"
export CARGO_NET_GIT_FETCH_WITH_CLI=true
cargo +nightly-2026-10-02 fetch --manifest-path worktrees/ziggurat/Cargo.toml
python3 scripts/prepare-crate-patches.py getrandom-0.4.3 nix-0.26.4 radium-0.7.0
export RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static'
cargo +nightly-2026-10-02 build --release \
    --manifest-path worktrees/ziggurat/Cargo.toml -p ziggurat-server \
    --target targets/lexra-stock-uclibc.json \
    -Z json-target-spec -Z build-std=std,panic_abort \
    --config "patch.crates-io.getrandom.path=\"$experiment_root/build/patched-crates/getrandom-0.4.3\"" \
    --config "patch.crates-io.nix.path=\"$experiment_root/build/patched-crates/nix-0.26.4\"" \
    --config "patch.crates-io.radium.path=\"$experiment_root/build/patched-crates/radium-0.7.0\""
