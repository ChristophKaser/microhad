#!/usr/bin/env bash
# Build a host-only Matter service with report-fixture hooks for integration tests.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"

bash scripts/build-openthread-transport.sh host
source scripts/rust-env.sh
export CARGO_TARGET_DIR="$project_root/build/rs-matter/service-test-target"
cargo +nightly-2026-10-02 build --locked --release \
    --manifest-path rs-matter-probe/Cargo.toml \
    --features controller-service-test-hooks --bin thread-matter
python3 scripts/check-matter-service-test-hooks.py \
    "$CARGO_TARGET_DIR/release/thread-matter" present
