#!/usr/bin/env bash
# Source this file from bash; rustup/cargo remain local to the experiment.
experiment_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export CARGO_HOME="$experiment_root/tools/cargo"
export RUSTUP_HOME="$experiment_root/tools/rustup"
export PATH="$CARGO_HOME/bin:$PATH"
