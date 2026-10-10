#!/usr/bin/env bash
#
# scripts/release-local.sh — stage a release locally (dev.md §10, stage 8.3).
#
#   default   build with the pinned container (static musl, the shipped
#             artifact) and write release/SHA256SUMS
#   --fast    build with the host toolchain (`make release`) instead; quicker
#             to iterate, but this is NOT the artifact that ships
#   --sign    sign release/SHA256SUMS with minisign (interactive passphrase;
#             it is never stored, never in argv)
#   --version override the version (default: git describe --tags --always)
#   --no-source  do not write the Corresponding Source tarball (iterate only)
#
# The signed release/SHA256SUMS(.minisig) are committed together with the tag;
# the built artifacts themselves are attached to the GitHub release, not
# committed. The signing key is read from ~/.config/berthpanel and never
# leaves the machine.
#
# Every release carries the Corresponding Source (GPL-3.0, dev.md §12.5): a
# source tarball built from the git tree, not a build artifact. `git archive`
# is deterministic and `release/` is excluded, so the tarball is identical
# whether it is generated at the source commit or the checksums commit.
#
# Overridable seams (used by the tests, and for re-checksumming a built file):
#   RELEASE_DIR       staging directory (default <repo>/release)
#   RELEASE_ARTIFACT  skip the build and use this prebuilt binary
#   RELEASE_MAKE      build tool (default: make)
#   RELEASE_REF       git ref the source tarball is built from (default HEAD)
#   MINISIGN          minisign binary (default: minisign)
#   BERTHPANEL_RELEASE_KEY_DIR  where release.key lives

set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "${script_dir}/.." && pwd)"

fast=0
sign=0
no_source=0
version=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --fast) fast=1 ;;
    --sign) sign=1 ;;
    --no-source) no_source=1 ;;
    --version)
      shift
      [ "$#" -gt 0 ] || { echo "release: --version needs a value" >&2; exit 2; }
      version="$1"
      ;;
    --help|-h)
      sed -n '2,25p' "$0"
      exit 0
      ;;
    *)
      echo "release: unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done

release_dir="${RELEASE_DIR:-${repo_root}/release}"
make_bin="${RELEASE_MAKE:-make}"
minisign_bin="${MINISIGN:-minisign}"
key="${BERTHPANEL_RELEASE_KEY_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/berthpanel}/release.key"

if [ -z "${version}" ]; then
  version="$(git -C "${repo_root}" describe --tags --always --dirty 2>/dev/null || echo v0.0.0-unknown)"
fi

if [ -n "${RELEASE_ARTIFACT:-}" ]; then
  artifact="${RELEASE_ARTIFACT}"
  echo "release: using prebuilt artifact ${artifact}"
elif [ "${fast}" -eq 1 ]; then
  echo "release: building with the host toolchain (NOT the shipped artifact)"
  "${make_bin}" -C "${repo_root}" release
  artifact="${repo_root}/build/release/berthpanel"
else
  echo "release: building the static artifact in the pinned container"
  "${make_bin}" -C "${repo_root}" musl
  artifact="${repo_root}/build/musl/berthpanel"
fi

if [ ! -f "${artifact}" ]; then
  echo "release: no artifact at ${artifact}" >&2
  exit 1
fi

mkdir -p "${release_dir}"
cp -f "${artifact}" "${release_dir}/berthpanel"

# install.sh rides along so the signed checksums cover the installer too
# (dev.md §10: install.sh is itself signed). It lands in stage 8.4.
if [ -f "${repo_root}/install.sh" ]; then
  cp -f "${repo_root}/install.sh" "${release_dir}/install.sh"
fi

# The public half of the release key ships as an asset so `install.sh` can
# fetch a trusted key from the release URL when none is supplied locally.
if [ -f "${repo_root}/docs/release.pub" ]; then
  cp -f "${repo_root}/docs/release.pub" "${release_dir}/release.pub"
fi

# Corresponding Source (GPL-3.0, dev.md §12.5): the tagged source tree, not a
# build artifact. `git archive` is deterministic; `release/` is excluded so
# the tarball does not contain its own checksums and is identical at the
# source commit and the checksums commit.
src_name=""
if [ "${no_source}" -eq 0 ]; then
  src_name="berthpanel-${version}-src.tar.gz"
  echo "release: writing Corresponding Source ${src_name}"
  git -C "${repo_root}" archive --format=tar \
    --prefix="berthpanel-${version}/" "${RELEASE_REF:-HEAD}" ':(exclude)release' \
    | gzip -n > "${release_dir}/${src_name}" || {
    echo "release: could not build the source tarball (need a git tree)" >&2
    exit 1
  }
  [ -s "${release_dir}/${src_name}" ] || {
    echo "release: the source tarball is empty" >&2
    exit 1
  }
fi

# Deterministic, sorted checksum list. Files are named relative to the release
# dir, so `sha256sum -c SHA256SUMS` works from inside it.
(
  cd "${release_dir}"
  : > SHA256SUMS
  for name in berthpanel install.sh release.pub ${src_name}; do
    if [ -f "${name}" ]; then
      sha256sum "${name}" >> SHA256SUMS
    fi
  done
)

notes="${release_dir}/notes/${version}.md"
if [ -f "${notes}" ]; then
  echo "release: notes ${notes}"
else
  echo "release: WARNING — no curated notes at release/notes/${version}.md" >&2
  echo "         the release will fall back to the tag body (dev.md §10)" >&2
fi

if [ "${sign}" -eq 1 ]; then
  if [ ! -f "${key}" ]; then
    echo "release: secret key ${key} not found; run scripts/keygen-release.sh" >&2
    exit 1
  fi
  if ! command -v "${minisign_bin}" >/dev/null 2>&1; then
    echo "release: ${minisign_bin} not found on PATH" >&2
    exit 1
  fi
  echo "release: signing SHA256SUMS (interactive; the passphrase is not stored)"
  rm -f "${release_dir}/SHA256SUMS.minisig"
  "${minisign_bin}" -S -s "${key}" -m "${release_dir}/SHA256SUMS" \
    -x "${release_dir}/SHA256SUMS.minisig" -t "berthpanel ${version}"
fi

echo
echo "release: version ${version}"
echo "release: staged into ${release_dir}"
cat "${release_dir}/SHA256SUMS"
echo
echo "Next steps:"
echo "  git add release/SHA256SUMS release/SHA256SUMS.minisig"
echo "  git commit -m 'release ${version}: signed checksums'"
echo "  git tag -a ${version} -m '${version}' && git push && git push origin ${version}"
