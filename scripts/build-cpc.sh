#!/usr/bin/env bash
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$experiment_root"
mkdir -p worktrees build logs
revision=$(git -C cpc-daemon rev-parse HEAD)
if [[ ! -d worktrees/cpc-daemon ]]; then
    git -C cpc-daemon worktree add --detach ../worktrees/cpc-daemon "$revision"
fi
[[ $(git -C worktrees/cpc-daemon rev-parse HEAD) == "$revision" ]] || {
    echo "CPC worktree does not match the pinned submodule" >&2
    exit 1
}
patches=(
    "$experiment_root/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/cpcd/tcp-bus.patch"
    "$experiment_root/patches/cpc-stock.patch"
)
for patch in "${patches[@]}"; do
    if ! git -C worktrees/cpc-daemon apply --reverse --check "$patch" 2>/dev/null; then
        git -C worktrees/cpc-daemon apply --check "$patch"
        git -C worktrees/cpc-daemon apply "$patch"
    fi
done
cmake -S worktrees/cpc-daemon -B build/cpc-stock \
    -DCMAKE_TOOLCHAIN_FILE="$experiment_root/cpc/stock-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_ENCRYPTION=OFF \
    -DUSE_LEGACY_GPIO_SYSFS=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
    -DCPC_SOCKET_DIR=/tmp
cmake --build build/cpc-stock -j 4
