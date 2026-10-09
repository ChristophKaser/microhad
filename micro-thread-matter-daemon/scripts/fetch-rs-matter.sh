#!/usr/bin/env bash
# Pinned Rust Matter source plus the production controller mailbox series.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$project_root"
source_dir="$project_root/micro-thread-matter-daemon/dependencies/rs-matter"
service_dir="$project_root/build/micro-thread-matter-daemon/service-source"
revision=1570356ecb938a9d19be032b93ca3bb487069e75
git -C "$project_root" submodule update --init micro-thread-matter-daemon/dependencies/rs-matter
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]] || {
    echo "Expected rs-matter source revision $revision" >&2; exit 1
}
[[ -z $(git -C "$source_dir" status --porcelain) ]] || {
    echo 'rs-matter source must be unmodified' >&2; exit 1
}
if [[ ! -e "$service_dir" ]]; then
    git -C "$source_dir" worktree add --detach "$service_dir" "$revision"
fi
python3 scripts/apply-mailbox-series.py "$service_dir" "$revision" \
    "$project_root/micro-thread-matter-daemon/patches/rs-matter-spake2p.patch" \
    "$project_root/micro-thread-matter-daemon/patches/rs-matter-peer-identity.patch" \
    "$project_root/micro-thread-matter-daemon/patches/rs-matter-attribute-notify.patch" \
    "$project_root/micro-thread-matter-daemon/patches/rs-matter-icd-capture.patch" \
    "$project_root/micro-thread-matter-daemon/patches/rs-matter-sessionless-icd.patch" \
    "$project_root/micro-thread-matter-daemon/patches/rs-matter-commissioning-region.patch"
echo "Pinned rs-matter $revision; controller mailbox series applied"
