#!/usr/bin/env bash
set -euo pipefail
experiment_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$experiment_root"
mkdir -p worktrees build logs
revision=$(git -C cpc-daemon rev-parse HEAD)
if [[ ! -d worktrees/cpc-daemon ]]; then
    git -C cpc-daemon worktree add --detach ../worktrees/cpc-daemon "$revision"
fi
python3 scripts/apply-mailbox-series.py worktrees/cpc-daemon "$revision" \
    "$experiment_root/patches/cpc-tcp-bus.patch" \
    "$experiment_root/patches/cpc-stock.patch"
cmake -S worktrees/cpc-daemon -B build/cpc-stock \
    -DCMAKE_TOOLCHAIN_FILE="$experiment_root/cpc/stock-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_ENCRYPTION=OFF \
    -DUSE_LEGACY_GPIO_SYSFS=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
    -DCPC_SOCKET_DIR=/tmp
cmake --build build/cpc-stock -j 4
