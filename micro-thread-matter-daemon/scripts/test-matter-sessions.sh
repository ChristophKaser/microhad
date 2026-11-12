#!/usr/bin/env bash
# Host-only tests of the production session mailbox and controller recovery.
set -euo pipefail
cd -P -- "$(dirname -- "$0")/../.."
bash micro-thread-matter-daemon/scripts/fetch-rs-matter.sh
source scripts/rust-env.sh
export CARGO_TARGET_DIR="$PWD/build/micro-thread-matter-daemon/controller-target"
library=(cargo +"$RUST_TOOLCHAIN" test --locked --release
    --manifest-path build/micro-thread-matter-daemon/service-source/Cargo.toml
    -p rs-matter --no-default-features --features 'os rustcrypto log max-sessions-64')
"${library[@]}" --lib transport::session::tests -- --nocapture
"${library[@]}" --test sessions --test case --test mrp
export CARGO_TARGET_DIR="$PWD/build/micro-thread-matter-daemon/target"
cargo +"$RUST_TOOLCHAIN" test --locked --release \
    --manifest-path micro-thread-matter-daemon/Cargo.toml --features controller-service \
    --lib devices::tests::lost_case_session_queues_recovery_without_waiting_for_report_timeout
