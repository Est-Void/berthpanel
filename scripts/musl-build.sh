#!/usr/bin/env bash
#
# scripts/musl-build.sh — the static musl release build (dev.md §3, stage 8.2).
#
# The host has no root and no musl toolchain, so the build runs in the pinned
# Arch container. Two supply-chain rules are enforced HERE, not left to the
# caller:
#
#   1. the image is pinned by DIGEST; any tag (including `latest`) is refused
#      outright — a floating tag in a build that produces signed artifacts is
#      the whole attack (dev.md §11);
#   2. argv arrays only: the container is invoked with an explicit argument
#      list, never a shell string (dev.md §4).
#
# The artifact is streamed out on the container's stdout (all build logs go to
# its stderr and on to ours), so the source is mounted read-only and no
# root-owned file is ever written into the working tree. The result is checked
# for staticness and the script FAILS if it is dynamic rather than shipping a
# bad artifact.
#
# Environment (all optional except CONTAINER):
#   CONTAINER           image reference; MUST keep its @sha256: pin
#   CONTAINER_RUNTIME   podman|docker path; auto-detected from PATH otherwise
#   MUSL_OUT            output directory (default build/musl)
#   MUSL_CC, MUSL_CXX   container toolchain (default musl-gcc, musl-g++)
#   MUSL_PACKAGES       container package set (default base-devel musl zlib)
#   SOURCE_DATE_EPOCH   forwarded into the container (default 0)

set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd)"

container="${CONTAINER:-}"
if [ -z "${container}" ]; then
  echo "musl: CONTAINER is not set" >&2
  exit 1
fi

# Digest-only. A tag here is refused, never resolved.
case "${container}" in
  *@sha256:*) ;;
  *)
    echo "musl: refusing '${container}': pin the image by digest (@sha256:...)," >&2
    echo "      never by tag (dev.md §3)" >&2
    exit 1
    ;;
esac

runtime="${CONTAINER_RUNTIME:-}"
if [ -z "${runtime}" ]; then
  for candidate in podman docker; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      runtime="$(command -v "${candidate}")"
      break
    fi
  done
fi
if [ -z "${runtime}" ]; then
  echo "musl: no container runtime found (install podman or docker)" >&2
  exit 1
fi

epoch="${SOURCE_DATE_EPOCH:-0}"
out_dir="${MUSL_OUT:-${repo_root}/build/musl}"
mkdir -p "${out_dir}"
artifact="${out_dir}/berthpanel"
tmp="${out_dir}/.berthpanel.tmp.$$"

echo "musl: image   ${container}"
echo "musl: runtime ${runtime}"
echo "musl: epoch   ${epoch}"

# stdout = artifact bytes only; stderr = logs. The read-only source mount means
# the container cannot touch the tree.
if ! "${runtime}" run --rm \
  --env "SOURCE_DATE_EPOCH=${epoch}" \
  --env "MUSL_CC=${MUSL_CC:-musl-gcc}" \
  --env "MUSL_CXX=${MUSL_CXX:-musl-g++}" \
  --env "MUSL_PACKAGES=${MUSL_PACKAGES:-base-devel musl zlib}" \
  --volume "${repo_root}:/src:ro" \
  "${container}" \
  /bin/sh /src/scripts/musl-inner.sh > "${tmp}"; then
  rm -f "${tmp}"
  echo "musl: the container build failed" >&2
  exit 1
fi

if [ ! -s "${tmp}" ]; then
  rm -f "${tmp}"
  echo "musl: the container produced no artifact" >&2
  exit 1
fi

# Staticness is the contract. `file` says "statically linked"; a dynamic ELF is
# a hard failure, never a warning (dev.md §11: fail loudly).
if ! file "${tmp}" | grep -q 'statically linked'; then
  echo "musl: artifact is not statically linked:" >&2
  file "${tmp}" >&2
  rm -f "${tmp}"
  exit 1
fi

mv -f "${tmp}" "${artifact}"
echo "musl: OK — ${artifact} ($(sha256sum "${artifact}" | cut -d' ' -f1))"
