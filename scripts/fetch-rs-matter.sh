#!/usr/bin/env bash
# Pinned Rust Matter source plus an isolated test-only visibility patch.
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
[[ $(git -C "$probe_dir" rev-parse HEAD) == "$revision" ]] || exit 1
if rg -q 'MAX_ICD_CHECK_IN_BYTES' "$probe_dir/rs-matter/src/lib.rs"; then
    echo 'Pinned rs-matter probe patch series is already applied'
    exit 0
fi
for patch_file in \
    "$project_root/patches/rs-matter-probe-spake2p.patch" \
    "$project_root/patches/rs-matter-probe-peer-identity.patch" \
    "$project_root/patches/rs-matter-probe-attribute-notify.patch" \
    "$project_root/patches/rs-matter-probe-icd-capture.patch" \
    "$project_root/patches/rs-matter-probe-sessionless-icd.patch"; do
    if ! git -C "$probe_dir" apply --reverse --check "$patch_file" 2>/dev/null; then
        git -C "$probe_dir" apply --check "$patch_file"
        git -C "$probe_dir" apply "$patch_file"
    fi
done
echo "Pinned rs-matter $revision; probe-only visibility, notification and ICD capture patches applied"
