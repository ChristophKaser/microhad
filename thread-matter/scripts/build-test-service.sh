#!/usr/bin/env bash
# Build a host-only Matter service with report-fixture hooks for integration tests.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$project_root"

bash thread-matter/scripts/build-openthread-transport.sh host
bash thread-matter/scripts/fetch-rs-matter.sh
source scripts/rust-env.sh
export CARGO_TARGET_DIR="$project_root/build/thread-matter/service-test-target"
cargo +$RUST_TOOLCHAIN build --locked --release \
    --manifest-path thread-matter/Cargo.toml \
    --features controller-service-test-hooks --bin thread-matter
python3 thread-matter/scripts/check-matter-service-test-hooks.py \
    "$CARGO_TARGET_DIR/release/thread-matter" present
