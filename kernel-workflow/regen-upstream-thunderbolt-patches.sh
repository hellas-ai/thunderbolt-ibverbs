#!/usr/bin/env bash
set -euo pipefail

# Refresh the 0101-0121 maintainer-tree patch series.
#
# To use a mirror, set THUNDERBOLT_REPO_URL to any repository containing the
# commits below. The generated patches are just git-format-patch output for the
# same range a GitHub/GitLab compare view would show.
#
# When the upstream series changes, bump THUNDERBOLT_PATCH_BASE,
# THUNDERBOLT_PATCH_HEAD, expected_count, and upstream-thunderbolt-next.nix
# together.

repo_url="${THUNDERBOLT_REPO_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/westeri/thunderbolt.git}"
ref="${THUNDERBOLT_REF:-next}"
base="${THUNDERBOLT_PATCH_BASE:-c866393eeb9c141342217b3c253719eeb7834575^}"
head="${THUNDERBOLT_PATCH_HEAD:-b6dd8fcfbc99dbf190e833ba6dff38a06d610e67}"
expected_count="${THUNDERBOLT_PATCH_COUNT:-21}"
cache="${THUNDERBOLT_REPO_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/thunderbolt-ibverbs/westeri-thunderbolt}"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
patch_dir="$script_dir/patches"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

if git -C "$cache" rev-parse --git-dir >/dev/null 2>&1; then
	git -C "$cache" remote set-url origin "$repo_url"
else
	mkdir -p "$(dirname "$cache")"
	git clone --filter=blob:none --no-checkout --single-branch --branch "$ref" "$repo_url" "$cache"
fi

git -C "$cache" fetch --filter=blob:none --prune origin "$ref"
git -C "$cache" format-patch --start-number=101 --output-directory "$tmp_dir" "$base..$head" >/dev/null

count="$(find "$tmp_dir" -maxdepth 1 -name '*.patch' | wc -l)"
if [ "$count" -ne "$expected_count" ]; then
	printf 'expected %s patches for %s..%s, got %s\n' "$expected_count" "$base" "$head" "$count" >&2
	exit 1
fi

find "$patch_dir" -maxdepth 1 -type f -name '01[0-9][0-9]-*.patch' -delete
mv "$tmp_dir"/*.patch "$patch_dir"/

printf 'refreshed %s upstream Thunderbolt patches in %s\n' "$count" "$patch_dir"
