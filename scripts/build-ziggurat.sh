#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
cd "$experiment_root"
ZIGGURAT_MULTIPAN=${ZIGGURAT_MULTIPAN:-1}
if [[ "$ZIGGURAT_MULTIPAN" != 0 && "$ZIGGURAT_MULTIPAN" != 1 ]]; then
    echo "ZIGGURAT_MULTIPAN must be 0 or 1" >&2
    exit 2
fi
mkdir -p worktrees build logs
source_revision=$(git -C ziggurat rev-parse HEAD)
build_worktree=worktrees/ziggurat
build_output=build/ziggurat
if [[ $ZIGGURAT_MULTIPAN == 1 ]]; then
    build_worktree=worktrees/ziggurat-hdlc
    build_output=build/ziggurat-hdlc
fi
if [[ ! -d ${build_worktree} ]]; then
    git -C ziggurat worktree add --detach "../$build_worktree" "$source_revision"
fi
mailboxes=()
if [[ $ZIGGURAT_MULTIPAN == 1 ]]; then
    mailboxes+=(
        patches/ziggurat-hdlc-multipan.patch
        patches/ziggurat-network-migration.patch
        patches/ziggurat-multipan-stock-compat.patch
        patches/ziggurat-multipan-syslog.patch
        patches/ziggurat-cobs-unix.patch
        patches/ziggurat-poll-diagnostics.patch
        patches/ziggurat-parent-announcement.patch
        patches/ziggurat-fresh-join-replay.patch
    )
else
    mailboxes+=(patches/ziggurat-stock-compat.patch patches/ziggurat-syslog.patch)
fi
mailbox_paths=()
for mailbox in "${mailboxes[@]}"; do mailbox_paths+=("$experiment_root/$mailbox"); done
python3 scripts/apply-mailbox-series.py "$build_worktree" "$source_revision" "${mailbox_paths[@]}"
python3 scripts/check-ziggurat-patches.py --build-worktree "$build_worktree" \
    --source-revision "$source_revision" --multipan "$ZIGGURAT_MULTIPAN"
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
    --config "patch.crates-io.getrandom.path=\"$experiment_root/build/patched-crates-mailbox/getrandom-0.4.3\"" \
    --config "patch.crates-io.nix.path=\"$experiment_root/build/patched-crates-mailbox/nix-0.26.4\"" \
    --config "patch.crates-io.radium.path=\"$experiment_root/build/patched-crates-mailbox/radium-0.7.0\""
if [[ $ZIGGURAT_MULTIPAN == 1 ]]; then
    target_binary="$experiment_root/$build_output/lexra-stock-uclibc/release/ziggurat"
    "$experiment_root/../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129/bin/mips-linux-strip" \
        -o "$target_binary.stripped" "$target_binary"
fi
