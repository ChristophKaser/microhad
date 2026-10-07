#!/usr/bin/env bash
# Source this file; every tool/state directory stays inside the experiment.
experiment_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export GECKO_SDK="${GECKO_SDK:-$experiment_root/dependencies/gecko-sdk}"
export ARM_GCC_DIR="${ARM_GCC_DIR:-$experiment_root/tools/silabs/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi}"
export PATH="$experiment_root/build/slc-cli:$ARM_GCC_DIR/bin:/usr/lib/jvm/java-17-openjdk-amd64/bin:$PATH"
export JAVA_TOOL_OPTIONS="-Duser.home=$experiment_root/tools/silabs"
