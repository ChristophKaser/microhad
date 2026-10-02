#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
mkdir -p "$experiment_root/tools"
curl --fail --location --connect-timeout 10 --max-time 120 \
    https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init \
    --output "$experiment_root/tools/rustup-init"
chmod +x "$experiment_root/tools/rustup-init"
"$experiment_root/tools/rustup-init" -y --no-modify-path --profile minimal \
    --default-toolchain nightly-2026-10-02 --component rust-src --component rustfmt
