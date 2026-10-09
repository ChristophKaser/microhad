#!/usr/bin/env bash
# Build a host-only Matter service with report-fixture hooks for integration tests.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$project_root"

bash micro-thread-matter-daemon/scripts/build-openthread-transport.sh host
bash micro-thread-matter-daemon/scripts/fetch-rs-matter.sh
source scripts/rust-env.sh
export CARGO_TARGET_DIR="$project_root/build/micro-thread-matter-daemon/service-test-target"
cargo +$RUST_TOOLCHAIN build --locked --release \
    --manifest-path micro-thread-matter-daemon/Cargo.toml \
    --features controller-service-test-hooks --bin mthread-matter
python3 micro-thread-matter-daemon/scripts/check-matter-service-test-hooks.py \
    "$CARGO_TARGET_DIR/release/mthread-matter" present
