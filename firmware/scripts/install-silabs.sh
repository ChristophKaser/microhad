#!/usr/bin/env bash
# Install only build tools, inside the ignored experiment tools directory.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
tools_dir="$experiment_root/tools/silabs"
mkdir -p "$tools_dir"
sdk_revision=461f90758bb18b3416bad77b1f1248f1a76dbeed
git -C "$experiment_root" submodule update --init firmware/dependencies/slc-cli firmware/dependencies/gecko-sdk
slc_source="$experiment_root/firmware/dependencies/slc-cli"
slc_worktree="$experiment_root/build/slc-cli"
slc_revision=$(git -C "$slc_source" rev-parse HEAD)
if [[ -e "$slc_worktree/.git" ]]; then
    [[ $(git -C "$slc_worktree" rev-parse HEAD) == "$slc_revision" ]] || {
        echo 'Existing SLC CLI worktree is not at the pinned submodule revision.' >&2; exit 1;
    }
else
    [[ ! -e "$slc_worktree" ]] || {
        echo "Refusing to replace existing SLC CLI worktree path: $slc_worktree" >&2; exit 1;
    }
    git -C "$slc_source" worktree add --detach "$slc_worktree" "$slc_revision"
fi
arm_archive="$tools_dir/arm-gcc.tar.xz"
if [[ ! -x "$tools_dir/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-gcc" ]]; then
    curl --fail --location --retry 2 \
        'https://developer.arm.com/-/media/Files/downloads/gnu/12.2.rel1/binrel/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi.tar.xz' \
        -o "$arm_archive"
    echo "84be93d0f9e96a15addd490b6e237f588c641c8afdf90e7610a628007fc96867  $arm_archive" | sha256sum --check
    tar xf "$arm_archive" -C "$tools_dir"
fi
sdk="$experiment_root/firmware/dependencies/gecko-sdk"
[[ $(git -C "$sdk" rev-parse HEAD) == "$sdk_revision" ]]
git -C "$sdk" sparse-checkout init --cone
git -C "$sdk" sparse-checkout set protocol/openthread platform \
    util/third_party/openthread util/third_party/mbedtls util/third_party/printf \
    util/silicon_labs util/plugin app/common
[[ -x "$slc_worktree/slc" ]]
command -v /usr/lib/jvm/java-17-openjdk-amd64/bin/java >/dev/null || {
    echo 'Java 17 is required; override the Java path in firmware/scripts/silabs-env.sh if needed.' >&2; exit 1;
}
echo 'Build tools installed. Source firmware/scripts/silabs-env.sh; no device was accessed.'
