#!/usr/bin/env bash
#
# scripts/repro-check.sh — local reproducibility check (dev.md §3, stage 8.1).
#
# Builds the release artifact twice from clean and requires the two hashes to
# match byte for byte, then asserts that no absolute build-directory path was
# baked into the binary. A mismatch means toolchain drift (or a fresh
# __DATE__/__TIME__/random build-id): investigate, never paper over it.
#
# The cross-path half of the guarantee — a build at a different absolute path
# hashes the same as the committed SHA256SUMS — is CI's job (stage 8.5), where
# the checkout path never matches the maintainer's.
#
# Writes only under build/ ; the source tree is left untouched.

set -euo pipefail

# Resolve the repository root from this script, so the check works from any
# working directory and never assumes $PWD.
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
cd "${repo_root}"

binary="build/release/berthpanel"

hash_of() {
  sha256sum "$1" | cut -d' ' -f1
}

# Parallel builds are the point: the object list is explicit and sorted, so
# `make -j` must not be able to reorder it (dev.md §3).
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

build_once() {
  make clean >/dev/null
  make -j"${jobs}" release >/dev/null
  [ -f "${binary}" ] || { echo "repro: ${binary} was not produced" >&2; exit 1; }
}

echo "repro: build #1"
build_once
first="$(hash_of "${binary}")"

echo "repro: build #2"
build_once
second="$(hash_of "${binary}")"

echo "  build #1: ${first}"
echo "  build #2: ${second}"

if [ "${first}" != "${second}" ]; then
  echo "repro: FAIL — the two builds differ (dev.md §3)" >&2
  exit 1
fi

# No absolute build path may survive in the artifact. -ffile-prefix-map is
# what enforces this; if it were dropped, DWARF/__FILE__ would leak $PWD.
if grep -aqF "${repo_root}" "${binary}"; then
  echo "repro: FAIL — the binary contains the build path ${repo_root}" >&2
  exit 1
fi

echo "repro: OK — ${first} (stable across two clean builds, no build path baked in)"
