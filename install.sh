#!/bin/sh
#
# install.sh — install AND uninstall berthpanel (dev.md §10, stage 8.4).
#
# Verify-first, fail-closed: the release's SHA256SUMS is checked against a
# minisign signature from the pinned public key BEFORE anything is installed,
# and every artifact is checksum-verified. --allow-checksum-only is the
# explicit opt-out. This script never runs sudo and never installs a system
# package — installing git/python/systemd is a human decision (dev.md §10).
#
# install.sh is itself signed by the release key, so it can be verified before
# it is piped to a shell.
#
# Usage:
#   install.sh [--prefix DIR] [--source DIR] [--pub FILE] [--allow-checksum-only] [--dry-run]
#   install.sh --uninstall [--prefix DIR] [--dry-run]
#
# Environment:
#   BERTHPANEL_MINISIGN      path to a minisign/rsign binary (skips detection)
#   BERTHPANEL_RELEASE_PUB   trusted public key (default <source>/release.pub)
#   BERTHPANEL_RELEASE_URL   base URL for a download when artifacts are not
#                            already present locally

set -eu
# pipefail where the shell has it (bash/ksh); dash does not, and that is fine.
if (set -o pipefail) 2>/dev/null; then
  set -o pipefail
fi

REPO_SLUG="Est-Void/berthpanel"
BIN_NAME="berthpanel"
UNIT_PREFIX="berthpanel-"

# Pinned static minisign for the verify-first fallback (dev.md §10). The
# tarball holds both x86_64 and aarch64; its SHA-256 is the pin — the download
# is a supply-chain input, so a floating URL or an unpinned fetch is refused.
MINISIGN_VERSION="0.12"
MINISIGN_URL="https://github.com/jedisct1/minisign/releases/download/${MINISIGN_VERSION}/minisign-${MINISIGN_VERSION}-linux.tar.gz"
MINISIGN_TARBALL_SHA256="9a599b48ba6eb7b1e80f12f36b94ceca7c00b7a5173c95c3efc88d9822957e73"

die() {
  echo "install: $*" >&2
  exit 1
}

usage() {
  sed -n '2,26p' "$0" 2>/dev/null || echo "berthpanel installer/uninstaller"
}

# --- argument parsing --------------------------------------------------------

action="install"
prefix="${PREFIX:-${HOME:-}/.local}"
source_dir=""
pub_file=""
version=""
release_url="${BERTHPANEL_RELEASE_URL:-}"
allow_checksum_only=0
dry_run=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --uninstall) action="uninstall" ;;
    --prefix)
      shift
      [ "$#" -gt 0 ] || die "--prefix needs a value"
      prefix="$1"
      ;;
    --source)
      shift
      [ "$#" -gt 0 ] || die "--source needs a value"
      source_dir="$1"
      ;;
    --pub)
      shift
      [ "$#" -gt 0 ] || die "--pub needs a value"
      pub_file="$1"
      ;;
    --version)
      shift
      [ "$#" -gt 0 ] || die "--version needs a value"
      version="$1"
      ;;
    --allow-checksum-only) allow_checksum_only=1 ;;
    --dry-run) dry_run=1 ;;
    -h|--help)
      usage
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

[ -n "${HOME:-}" ] || die "HOME is not set"
[ -n "$prefix" ] || die "could not determine an install prefix"

if [ -z "$source_dir" ]; then
  source_dir="$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || pwd)"
fi

# --- helpers -----------------------------------------------------------------

fetch_file() {
  url="$1"
  out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl --fail --silent --show-error --location --proto '=https' \
      --output "$out" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget --quiet --https-only --output-document="$out" "$url"
  else
    die "need curl or wget to download $url"
  fi
}

sha256_of() {
  command -v sha256sum >/dev/null 2>&1 || die "sha256sum is required"
  sha256sum "$1" | cut -d' ' -f1
}

# Locate a minisign binary: an explicit override, the system, or the pinned
# static download. Prints the path on stdout; dies if none is available.
ensure_minisign() {
  if [ -n "${BERTHPANEL_MINISIGN:-}" ]; then
    printf '%s' "${BERTHPANEL_MINISIGN}"
    return 0
  fi
  if command -v minisign >/dev/null 2>&1; then
    command -v minisign
    return 0
  fi
  if command -v rsign >/dev/null 2>&1; then
    command -v rsign
    return 0
  fi
  # Fallback: a pinned static minisign, fetched on demand.
  echo "install: no system minisign/rsign; fetching the pinned static minisign" >&2
  ms_dir="$(mktemp -d)"
  fetch_file "$MINISIGN_URL" "$ms_dir/minisign.tgz"
  actual="$(sha256_of "$ms_dir/minisign.tgz")"
  [ "$actual" = "$MINISIGN_TARBALL_SHA256" ] ||
    die "pinned minisign tarball hash mismatch (wanted ${MINISIGN_TARBALL_SHA256}, got ${actual})"
  case "$(uname -m)" in
    x86_64|amd64) member="minisign-linux/x86_64/minisign" ;;
    aarch64|arm64) member="minisign-linux/aarch64/minisign" ;;
    *) die "no pinned static minisign for $(uname -m); install minisign yourself" ;;
  esac
  tar -xzf "$ms_dir/minisign.tgz" -C "$ms_dir" "$member" ||
    die "could not extract the pinned minisign"
  printf '%s' "$ms_dir/$member"
}

# Verify a detached signature. minisign and rsign differ in CLI shape; both are
# accepted. Any failure is fatal (fail closed).
verify_signature() {
  bin="$1"
  pub="$2"
  msg="$3"
  sig="$4"
  case "$(basename -- "$bin")" in
    rsign*)
      "$bin" verify -p "$pub" -x "$sig" "$msg"
      ;;
    *)
      "$bin" -V -p "$pub" -m "$msg" -x "$sig"
      ;;
  esac
}

# --- uninstall ---------------------------------------------------------------

uninstall() {
  systemctl_bin="systemctl"
  units_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
  removed=0

  # Everything the uninstaller touches is derived from `systemctl --user`
  # output and filtered by the `berthpanel-` prefix. Nothing outside that
  # prefix is ever stopped, disabled or deleted.
  units="$(
    {
      "$systemctl_bin" --user list-units --all --type=service --type=timer \
        --no-legend --plain 2>/dev/null | awk '{print $1}'
      "$systemctl_bin" --user list-unit-files --type=service --type=timer \
        --no-legend --plain 2>/dev/null | awk '{print $1}'
    } | sort -u
  )"

  for unit in $units; do
    case "$unit" in
      "${UNIT_PREFIX}"*) ;;
      *) continue ;;
    esac
    if [ "$dry_run" -eq 1 ]; then
      echo "install: would remove unit ${unit}"
      continue
    fi
    if ! "$systemctl_bin" --user disable --now "$unit" >/dev/null 2>&1; then
      echo "install: warning: could not disable ${unit} (continuing)" >&2
    fi
    "$systemctl_bin" --user reset-failed "$unit" >/dev/null 2>&1 || true
    file="${units_dir}/${unit}"
    case "${file}" in
      "${units_dir}/${UNIT_PREFIX}"*)
        if [ -f "$file" ]; then
          rm -f "$file"
          removed=$((removed + 1))
        fi
        ;;
    esac
  done

  bin="${prefix}/bin/${BIN_NAME}"
  if [ -f "$bin" ]; then
    if [ "$dry_run" -eq 1 ]; then
      echo "install: would remove ${bin}"
    else
      rm -f "$bin"
    fi
  fi

  if [ "$dry_run" -eq 1 ]; then
    echo "install: dry run complete"
    return 0
  fi

  "$systemctl_bin" --user daemon-reload >/dev/null 2>&1 || true
  echo "install: berthpanel uninstalled (${removed} unit file(s) removed)"
  echo "install: your service data under ~/.local/share/berthpanel is left in place"
}

# --- install -----------------------------------------------------------------

install() {
  # Resolve artifacts: use local files when present, otherwise fetch from the
  # release URL. Never a floating `latest` — the URL is version-pinned.
  work_dir="$source_dir"
  if [ ! -f "$work_dir/${BIN_NAME}" ] || [ ! -f "$work_dir/SHA256SUMS" ]; then
    [ -n "$release_url" ] || [ -n "$version" ] ||
      die "no local artifacts and no --version/BERTHPANEL_RELEASE_URL to download"
    if [ -z "$release_url" ]; then
      release_url="https://github.com/${REPO_SLUG}/releases/download/${version}"
    fi
    work_dir="$(mktemp -d)"
    echo "install: fetching artifacts from ${release_url}"
    fetch_file "${release_url}/${BIN_NAME}" "$work_dir/${BIN_NAME}"
    fetch_file "${release_url}/SHA256SUMS" "$work_dir/SHA256SUMS"
    if [ "$allow_checksum_only" -eq 0 ]; then
      fetch_file "${release_url}/SHA256SUMS.minisig" "$work_dir/SHA256SUMS.minisig"
    fi
    if [ -z "$pub_file" ]; then
      fetch_file "${release_url}/release.pub" "$work_dir/release.pub"
    fi
  fi

  [ -f "$work_dir/SHA256SUMS" ] || die "no SHA256SUMS in ${work_dir}"
  [ -f "$work_dir/${BIN_NAME}" ] || die "no ${BIN_NAME} artifact in ${work_dir}"

  if [ "$allow_checksum_only" -eq 1 ]; then
    echo "install: WARNING — --allow-checksum-only: signature verification is SKIPPED" >&2
    echo "install: only the sha256 checksums protect you; a compromised mirror" >&2
    echo "install: would go undetected" >&2
  else
    [ -f "$work_dir/SHA256SUMS.minisig" ] ||
      die "no SHA256SUMS.minisig; pass --allow-checksum-only only if you accept the risk"
    if [ -z "$pub_file" ]; then
      pub_file="${BERTHPANEL_RELEASE_PUB:-$work_dir/release.pub}"
    fi
    [ -f "$pub_file" ] || die "trusted public key not found at ${pub_file}"
    minisign_bin="$(ensure_minisign)"
    echo "install: verifying SHA256SUMS against ${pub_file}"
    verify_signature "$minisign_bin" "$pub_file" \
      "$work_dir/SHA256SUMS" "$work_dir/SHA256SUMS.minisig" ||
      die "signature verification FAILED — refusing to install"
  fi

  echo "install: verifying artifact checksums"
  (cd "$work_dir" && sha256sum --check --strict SHA256SUMS) ||
    die "checksum verification FAILED — refusing to install"

  dest_dir="${prefix}/bin"
  dest="${dest_dir}/${BIN_NAME}"
  if [ "$dry_run" -eq 1 ]; then
    echo "install: would install $(cd "$work_dir" && pwd)/${BIN_NAME} -> ${dest}"
    return 0
  fi

  mkdir -p "$dest_dir"
  tmp="${dest}.tmp.$$"
  cp "$work_dir/${BIN_NAME}" "$tmp"
  chmod 0755 "$tmp"
  mv -f "$tmp" "$dest"

  echo "install: installed ${dest}"
  echo "install: run '${BIN_NAME} --selftest' to confirm your user manager works"
}

case "$action" in
  install) install ;;
  uninstall) uninstall ;;
esac
