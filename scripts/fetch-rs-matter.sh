#!/usr/bin/env bash
# Pinned Rust Matter source plus an isolated test-only mailbox series.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$project_root/dependencies/rs-matter"
probe_dir="$project_root/build/rs-matter/probe-source"
revision=1570356ecb938a9d19be032b93ca3bb487069e75
git -C "$project_root" submodule update --init dependencies/rs-matter
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]] || {
    echo "Expected rs-matter source revision $revision" >&2; exit 1
}
[[ -z $(git -C "$source_dir" status --porcelain) ]] || {
    echo 'rs-matter source must be unmodified' >&2; exit 1
}
if [[ ! -e "$probe_dir" ]]; then
    git -C "$source_dir" worktree add --detach "$probe_dir" "$revision"
fi
python3 scripts/apply-mailbox-series.py "$probe_dir" "$revision" \
    "$project_root/patches/rs-matter-probe-spake2p.patch" \
    "$project_root/patches/rs-matter-probe-peer-identity.patch" \
    "$project_root/patches/rs-matter-probe-attribute-notify.patch" \
    "$project_root/patches/rs-matter-probe-icd-capture.patch" \
    "$project_root/patches/rs-matter-probe-sessionless-icd.patch" \
    "$project_root/patches/rs-matter-probe-commissioning-region.patch"
echo "Pinned rs-matter $revision; probe mailbox series applied"
