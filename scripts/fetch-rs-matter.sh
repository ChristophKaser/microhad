#!/usr/bin/env bash
# Pinned Rust Matter source plus an isolated test-only visibility patch.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$project_root/build/rs-matter/source"
probe_dir="$project_root/build/rs-matter/probe-source"
revision=1570356ecb938a9d19be032b93ca3bb487069e75
if [[ ! -e "$source_dir" ]]; then
    git clone --no-checkout https://github.com/project-chip/rs-matter.git "$source_dir"
    git -C "$source_dir" checkout --detach "$revision"
fi
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]] || {
    echo "Expected rs-matter source revision $revision" >&2; exit 1
}
[[ -z $(git -C "$source_dir" status --porcelain) ]] || {
    echo 'rs-matter source must be unmodified' >&2; exit 1
}
if [[ ! -e "$probe_dir" ]]; then
    git -C "$source_dir" worktree add --detach "$probe_dir" "$revision"
fi
[[ $(git -C "$probe_dir" rev-parse HEAD) == "$revision" ]] || exit 1
for patch_file in \
    "$project_root/patches/rs-matter-probe-spake2p.patch" \
    "$project_root/patches/rs-matter-probe-peer-identity.patch" \
    "$project_root/patches/rs-matter-probe-attribute-notify.patch"; do
    if ! git -C "$probe_dir" apply --reverse --check "$patch_file" 2>/dev/null; then
        git -C "$probe_dir" apply --check "$patch_file"
        git -C "$probe_dir" apply "$patch_file"
    fi
done
echo "Pinned rs-matter $revision; probe-only visibility and notification patches applied"
