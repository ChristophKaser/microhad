#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
cd "$experiment_root"
mkdir -p worktrees build logs
source_revision=$(git -C ziggurat rev-parse HEAD)
build_worktree=worktrees/ziggurat
build_output=build/ziggurat
if [[ ${ZIGGURAT_MULTIPAN:-0} == 1 ]]; then
    build_worktree=worktrees/ziggurat-hdlc
    build_output=build/ziggurat-hdlc
fi
if [[ ! -d ${build_worktree} ]]; then
    git -C ziggurat worktree add --detach "../$build_worktree" "$source_revision"
fi
if [[ $(git -C ${build_worktree} rev-parse HEAD) != "$source_revision" ]]; then
    echo "Build worktree does not match the pinned Ziggurat revision" >&2
    exit 1
fi
if [[ ${ZIGGURAT_MULTIPAN:-0} == 1 ]]; then
    if ! git -C "$build_worktree" apply --reverse --check --exclude=crates/ziggurat-server/src/main.rs "$experiment_root/patches/ziggurat-hdlc-multipan.patch" 2>/dev/null; then
        git -C "$build_worktree" apply --check "$experiment_root/patches/ziggurat-hdlc-multipan.patch"
        git -C "$build_worktree" apply "$experiment_root/patches/ziggurat-hdlc-multipan.patch"
    fi
    if ! git -C "$build_worktree" apply --reverse --check "$experiment_root/patches/ziggurat-network-migration.patch" 2>/dev/null; then
        git -C "$build_worktree" apply --check "$experiment_root/patches/ziggurat-network-migration.patch"
        git -C "$build_worktree" apply "$experiment_root/patches/ziggurat-network-migration.patch"
    fi
fi
if ! git -C ${build_worktree} apply --reverse --check \
    "$experiment_root/patches/ziggurat-stock-compat.patch" 2>/dev/null; then
    git -C ${build_worktree} apply --check "$experiment_root/patches/ziggurat-stock-compat.patch"
    git -C ${build_worktree} apply "$experiment_root/patches/ziggurat-stock-compat.patch"
fi
if [[ ${ZIGGURAT_MULTIPAN:-0} == 1 ]]; then
    if ! git -C "$build_worktree" apply --reverse --check "$experiment_root/patches/ziggurat-cobs-unix.patch" 2>/dev/null; then
        git -C "$build_worktree" apply --check "$experiment_root/patches/ziggurat-cobs-unix.patch"
        git -C "$build_worktree" apply "$experiment_root/patches/ziggurat-cobs-unix.patch"
    fi
    if ! git -C "$build_worktree" apply --reverse --check "$experiment_root/patches/ziggurat-poll-diagnostics.patch" 2>/dev/null; then
        git -C "$build_worktree" apply --check "$experiment_root/patches/ziggurat-poll-diagnostics.patch"
        git -C "$build_worktree" apply "$experiment_root/patches/ziggurat-poll-diagnostics.patch"
    fi
    if ! git -C "$build_worktree" apply --reverse --check "$experiment_root/patches/ziggurat-parent-announcement.patch" 2>/dev/null; then
        git -C "$build_worktree" apply --check "$experiment_root/patches/ziggurat-parent-announcement.patch"
        git -C "$build_worktree" apply "$experiment_root/patches/ziggurat-parent-announcement.patch"
    fi
fi
bash scripts/make-llvm-target.sh
export CARGO_TARGET_DIR="$experiment_root/${build_output}"
export CARGO_NET_GIT_FETCH_WITH_CLI=true
cargo +nightly-2026-10-02 fetch --manifest-path ${build_worktree}/Cargo.toml
python3 scripts/prepare-crate-patches.py getrandom-0.4.3 nix-0.26.4 radium-0.7.0
export RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static'
cargo +nightly-2026-10-02 build --release \
    --manifest-path ${build_worktree}/Cargo.toml -p ziggurat-server \
    --target targets/lexra-stock-uclibc.json \
    -Z json-target-spec -Z build-std=std,panic_abort \
    --config "patch.crates-io.getrandom.path=\"$experiment_root/build/patched-crates/getrandom-0.4.3\"" \
    --config "patch.crates-io.nix.path=\"$experiment_root/build/patched-crates/nix-0.26.4\"" \
    --config "patch.crates-io.radium.path=\"$experiment_root/build/patched-crates/radium-0.7.0\""
if [[ ${ZIGGURAT_MULTIPAN:-0} == 1 ]]; then
    target_binary="$experiment_root/$build_output/lexra-stock-uclibc/release/ziggurat"
    "$experiment_root/../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129/bin/mips-linux-strip" \
        -o "$target_binary.stripped" "$target_binary"
fi
