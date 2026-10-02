#!/usr/bin/env bash
# Install only build tools, inside the ignored experiment tools directory.
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
tools_dir="$experiment_root/tools/silabs"
mkdir -p "$tools_dir"
slc_revision=a2cef4434600379695bef23bfbdcc8e604f7f305
sdk_revision=90fc93e1f95a10c981f0e49ca6787192e82fd8f2
if [[ ! -x "$tools_dir/circuitpython_slc_cli_linux-main/slc" ]]; then
    curl --fail --location --retry 2 \
        "https://codeload.github.com/SiliconLabsSoftware/circuitpython_slc_cli_linux/tar.gz/$slc_revision" \
        -o "$tools_dir/slc-pinned.tar.gz"
    mkdir -p "$tools_dir/circuitpython_slc_cli_linux-main"
    tar xf "$tools_dir/slc-pinned.tar.gz" --strip-components=1 -C "$tools_dir/circuitpython_slc_cli_linux-main"
fi
arm_archive="$tools_dir/arm-gcc.tar.xz"
if [[ ! -x "$tools_dir/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-gcc" ]]; then
    curl --fail --location --retry 2 \
        'https://developer.arm.com/-/media/Files/downloads/gnu/12.2.rel1/binrel/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi.tar.xz' \
        -o "$arm_archive"
    echo "84be93d0f9e96a15addd490b6e237f588c641c8afdf90e7610a628007fc96867  $arm_archive" | sha256sum --check
    tar xf "$arm_archive" -C "$tools_dir"
fi
if [[ ! -d "$tools_dir/gecko_sdk/.git" ]]; then
    git clone --depth 1 --branch v4.5.0 --filter=blob:none --no-checkout \
        https://github.com/SiliconLabs/gecko_sdk.git "$tools_dir/gecko_sdk"
    git -C "$tools_dir/gecko_sdk" sparse-checkout init --cone
    git -C "$tools_dir/gecko_sdk" sparse-checkout set protocol/openthread platform \
        util/third_party/openthread util/third_party/mbedtls util/third_party/printf \
        util/silicon_labs util/plugin app/common
    git -C "$tools_dir/gecko_sdk" checkout
fi
[[ $(git -C "$tools_dir/gecko_sdk" rev-parse HEAD) == "$sdk_revision" ]]
command -v /usr/lib/jvm/java-17-openjdk-amd64/bin/java >/dev/null || {
    echo 'Java 17 is required; override the Java path in scripts/silabs-env.sh if needed.' >&2; exit 1;
}
echo 'Build tools installed. Source scripts/silabs-env.sh; no device was accessed.'
