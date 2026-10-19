#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../../scripts/rust-env.sh"
cd "$experiment_root"
ZIGGURAT_MULTIPAN=${ZIGGURAT_MULTIPAN:-1}
if [[ "$ZIGGURAT_MULTIPAN" != 0 && "$ZIGGURAT_MULTIPAN" != 1 ]]; then
    echo "ZIGGURAT_MULTIPAN must be 0 or 1" >&2
    exit 2
fi
mkdir -p worktrees build logs
source_revision=$(git -C ziggurat/dependencies/ziggurat rev-parse HEAD)
build_worktree=worktrees/ziggurat-stock
build_output=build/ziggurat
if [[ $ZIGGURAT_MULTIPAN == 1 ]]; then
    build_worktree=worktrees/ziggurat-multipan
    build_output=build/ziggurat-hdlc
fi
if [[ ! -d ${build_worktree} ]]; then
    git -C ziggurat/dependencies/ziggurat worktree add --detach "$experiment_root/$build_worktree" "$source_revision"
fi
mailboxes=()
if [[ $ZIGGURAT_MULTIPAN == 1 ]]; then
    mailboxes+=(
        ziggurat/patches/ziggurat-hdlc-multipan.patch
        ziggurat/patches/ziggurat-network-migration.patch
        ziggurat/patches/ziggurat-multipan-stock-compat.patch
        ziggurat/patches/ziggurat-multipan-syslog.patch
        ziggurat/patches/ziggurat-cobs-unix.patch
        ziggurat/patches/ziggurat-poll-diagnostics.patch
        ziggurat/patches/ziggurat-parent-announcement.patch
        ziggurat/patches/ziggurat-fresh-join-replay.patch
    )
else
    mailboxes+=(ziggurat/patches/ziggurat-stock-compat.patch ziggurat/patches/ziggurat-syslog.patch)
fi
mailboxes+=(ziggurat/patches/ziggurat-syslog-fragments.patch ziggurat/patches/ziggurat-async-syslog.patch)
mailboxes+=(ziggurat/patches/ziggurat-end-device-relay.patch)
if [ "$ZIGGURAT_MULTIPAN" = 1 ]; then
    mailboxes+=(ziggurat/patches/ziggurat-radio-trace.patch ziggurat/patches/ziggurat-thread-tcp.patch)
fi
mailbox_paths=()
for mailbox in "${mailboxes[@]}"; do mailbox_paths+=("$experiment_root/$mailbox"); done
python3 scripts/apply-mailbox-series.py "$build_worktree" "$source_revision" "${mailbox_paths[@]}"
python3 ziggurat/scripts/check-ziggurat-patches.py --build-worktree "$build_worktree" \
    --source-revision "$source_revision" --multipan "$ZIGGURAT_MULTIPAN"
build_output="build/${TARGET_BUILD_SUBDIR}${build_output#build/}"
python3 scripts/toolchain-build.py "$experiment_root/$build_output/$TARGET_SPEC_NAME"
bash scripts/make-llvm-target.sh
export CARGO_TARGET_DIR="$experiment_root/${build_output}"
export CARGO_NET_GIT_FETCH_WITH_CLI=true
cargo +$RUST_TOOLCHAIN fetch --manifest-path ${build_worktree}/Cargo.toml
python3 scripts/prepare-crate-patches.py getrandom-0.4.3 nix-0.26.4 radium-0.7.0
export RUSTFLAGS='-C target-feature=+crt-static -C relocation-model=static'
if [[ "$LINK_MODE" == dynamic ]]; then
    RUSTFLAGS=${RUSTFLAGS/-C target-feature=+crt-static -C relocation-model=static/-C target-feature=-crt-static -C relocation-model=pic}
fi
cargo +$RUST_TOOLCHAIN build --release \
    --manifest-path ${build_worktree}/Cargo.toml -p ziggurat-server \
    --target "targets/$TARGET_SPEC_NAME.json" \
    -Z json-target-spec -Z build-std=std,panic_abort \
    --config "patch.crates-io.getrandom.path=\"$experiment_root/build/patched-crates-mailbox/getrandom-0.4.3\"" \
    --config "patch.crates-io.nix.path=\"$experiment_root/build/patched-crates-mailbox/nix-0.26.4\"" \
    --config "patch.crates-io.radium.path=\"$experiment_root/build/patched-crates-mailbox/radium-0.7.0\""
if [[ $ZIGGURAT_MULTIPAN == 1 ]]; then
    target_binary="$experiment_root/$build_output/$TARGET_SPEC_NAME/release/ziggurat"
    "$LLVM_STRIP" --keep-section=.gnu.attributes \
        -o "$target_binary.stripped" "$target_binary"
    python3 scripts/verify-gateway-target.py --link-mode "$LINK_MODE" "$target_binary.stripped"
fi

python3 scripts/verify-gateway-target.py --link-mode "$LINK_MODE" "$experiment_root/$build_output/$TARGET_SPEC_NAME/release/ziggurat"
