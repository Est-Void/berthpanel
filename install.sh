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
# it is piped to a shell. Piping it straight from the repo's default branch
# (`curl -fsSL <raw-url>/install.sh | sh`) trusts TLS + GitHub for the SCRIPT
# itself; the release artifacts it installs are still verified (below).
#
# Usage:
#   install.sh [--prefix DIR] [--source DIR] [--pub FILE] [--version V]
#              [--allow-checksum-only] [--dry-run] [--add-to-path|--no-add-to-path]
#              [--enable-linger] [--selftest]
#   install.sh --uninstall [--prefix DIR] [--dry-run]
#
# Environment:
#   BERTHPANEL_MINISIGN      path to a minisign/rsign binary (skips detection)
#   BERTHPANEL_RELEASE_PUB   trusted public key (default <source>/release.pub)
#   BERTHPANEL_RELEASE_URL   base URL for the release artifacts (default: the
#                            pinned release below); when artifacts are not
#                            already present locally they are fetched from it
#   BERTHPANEL_PROFILE_FILE  shell profile edited by --add-to-path

set -eu
# pipefail where the shell has it (bash/ksh); dash does not, and that is fine.
if (set -o pipefail) 2>/dev/null; then
  set -o pipefail
fi

REPO_SLUG="Est-Void/berthpanel"
BIN_NAME="berthpanel"
UNIT_PREFIX="berthpanel-"

# The installer is served from the repo's default branch (the README one-liner);
# the artifacts it fetches are the ones attached to this exact release. Bump
# DEFAULT_VERSION together with the tag — never a floating `latest` (dev.md §10).
DEFAULT_BRANCH="main"
DEFAULT_VERSION="v0.1.0-alpha"
DEFAULT_RELEASE_URL="https://github.com/${REPO_SLUG}/releases/download/${DEFAULT_VERSION}"

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
  # Embedded so it works under `curl | sh`, where there is no script file on
  # disk and `$0` is just the shell name (a `sed ... "$0"` would fail).
  cat <<EOF
install.sh — install AND uninstall berthpanel.

Usage:
  install.sh [--prefix DIR] [--source DIR] [--pub FILE] [--version V]
             [--allow-checksum-only] [--dry-run]
             [--add-to-path|--no-add-to-path] [--enable-linger] [--selftest]
  install.sh --uninstall [--prefix DIR] [--dry-run]

One-liner (the script itself is not verified this way; see the README Install
section for the verify-first alternative):
  curl -fsSL https://raw.githubusercontent.com/${REPO_SLUG}/${DEFAULT_BRANCH}/install.sh | sh

Verify-first, fail-closed: the release's SHA256SUMS is checked against a
minisign signature before anything is installed. --allow-checksum-only is the
explicit opt-out. No sudo, no system packages.

After installing, the script checks git/python3/systemd, reports how to make
systemd user services survive logout, and can run --selftest. With the default
prefix (~/.local), the PATH block is added unless --no-add-to-path is passed.

Environment:
  BERTHPANEL_MINISIGN     minisign/rsign binary (skips detection)
  BERTHPANEL_RELEASE_PUB  trusted public key (default <source>/release.pub)
  BERTHPANEL_RELEASE_URL  base URL for release artifacts
  BERTHPANEL_PROFILE_FILE shell profile edited by --add-to-path
EOF
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
add_to_path="auto"
enable_linger=0
run_selftest=0
profile_file="${BERTHPANEL_PROFILE_FILE:-}"

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
    --add-to-path) add_to_path=1 ;;
    --no-add-to-path) add_to_path=0 ;;
    --enable-linger) enable_linger=1 ;;
    --selftest) run_selftest=1 ;;
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
  # Under `curl | sh` there is no script file: $0 is the shell name, and
  # dirname on it is meaningless. Only derive a directory when $0 is a real
  # file; otherwise fall back to the current directory.
  if [ -f "$0" ]; then
    source_dir="$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || pwd)"
  else
    source_dir="$(pwd)"
  fi
fi

# --- helpers -----------------------------------------------------------------

fetch_file() {
  url="$1"
  out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl --fail --silent --show-error --location --proto '=https' \
      --output "$out" "$url" || die "could not download ${url}"
  elif command -v wget >/dev/null 2>&1; then
    wget --quiet --https-only --output-document="$out" "$url" ||
      die "could not download ${url}"
  else
    die "need curl or wget to download ${url}"
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

# --- install experience ------------------------------------------------------

have_command() {
  command -v "$1" >/dev/null 2>&1
}

current_user() {
  if command -v id >/dev/null 2>&1; then
    id -un 2>/dev/null || true
  elif [ -n "${USER:-}" ]; then
    printf '%s' "$USER"
  fi
}

package_hint() {
  if have_command apt-get; then
    echo "sudo apt-get install git python3 systemd"
  elif have_command dnf; then
    echo "sudo dnf install git python3 systemd"
  elif have_command pacman; then
    echo "sudo pacman -S git python systemd"
  elif have_command zypper; then
    echo "sudo zypper install git python3 systemd"
  elif have_command apk; then
    echo "sudo apk add git python3 systemd"
  else
    echo "install git, python3 >= 3.10, systemctl and systemd-run with your package manager"
  fi
}

check_runtime_deps() {
  missing=""
  for tool in git python3 systemctl systemd-run; do
    if ! have_command "$tool"; then
      missing="$missing $tool"
    fi
  done
  if [ -n "$missing" ]; then
    echo "install: warning: missing runtime tools:$missing" >&2
    echo "install: berthpanel is installed, but deploys need them." >&2
    echo "install: install them yourself (this script never runs sudo):" >&2
    echo "  $(package_hint)" >&2
  fi
  if have_command python3 &&
    ! python3 -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
    echo "install: warning: python3 is older than 3.10; some builds will refuse to run" >&2
  fi
  if have_command systemctl &&
    ! systemctl --user show-environment >/dev/null 2>&1; then
    echo "install: warning: systemd --user is not reachable from this shell" >&2
    echo "install: log in to a normal user session before deploying or running --selftest" >&2
  fi
}

profile_file_for_shell() {
  if [ -n "$profile_file" ]; then
    printf '%s' "$profile_file"
    return 0
  fi
  case "$(basename -- "${SHELL:-sh}")" in
    zsh)
      printf '%s' "${ZDOTDIR:-$HOME}/.zshrc"
      ;;
    bash)
      if [ -f "$HOME/.bash_profile" ] && [ ! -f "$HOME/.bashrc" ]; then
        printf '%s' "$HOME/.bash_profile"
      else
        printf '%s' "$HOME/.bashrc"
      fi
      ;;
    *)
      printf '%s' "$HOME/.profile"
      ;;
  esac
}

path_has_dir() {
  case ":${PATH:-}:" in
    *":$1:"*) return 0 ;;
    *) return 1 ;;
  esac
}

shell_single_quote() {
  escaped="$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
  printf "'%s'" "$escaped"
}

remove_path_block() {
  file="$1"
  [ -f "$file" ] || return 0
  tmp="${file}.berthpanel.$$"
  if ! awk '
    $0 == "# >>> berthpanel installer: PATH >>>" { skip = 1; next }
    $0 == "# <<< berthpanel installer: PATH <<<" { skip = 0; next }
    skip != 1 { print }
  ' "$file" > "$tmp"; then
    rm -f "$tmp"
    return 1
  fi
  cat "$tmp" > "$file"
  rm -f "$tmp"
}

write_path_block() {
  file="$1"
  dir="$2"
  remove_path_block "$file" || return 1
  {
    printf '%s\n' "# >>> berthpanel installer: PATH >>>"
    printf 'PATH=%s:"$PATH"\n' "$(shell_single_quote "$dir")"
    printf 'export PATH\n'
    printf '%s\n' "# <<< berthpanel installer: PATH <<<"
  } >> "$file" || return 1
}

maybe_add_to_path() {
  dir="$1"
  if path_has_dir "$dir"; then
    echo "install: ${dir} is already on PATH"
    return 0
  fi
  if [ "$add_to_path" = "auto" ]; then
    if [ "$prefix" = "${HOME}/.local" ]; then
      add_to_path=1
    else
      add_to_path=0
    fi
  fi
  if [ "$add_to_path" -eq 0 ]; then
    echo "install: ${dir} is not on PATH; add it for this shell with:"
    echo "  export PATH=$(shell_single_quote "$dir"):\"\$PATH\""
    echo "install: or rerun with --add-to-path to update a shell profile"
    return 0
  fi
  profile="$(profile_file_for_shell)"
  if [ -z "$profile" ]; then
    echo "install: warning: could not determine a shell profile; add ${dir} to PATH yourself" >&2
    return 0
  fi
  if [ "$dry_run" -eq 1 ]; then
    echo "install: would add ${dir} to PATH in ${profile}"
    return 0
  fi
  mkdir -p "$(dirname -- "$profile")" 2>/dev/null || true
  if write_path_block "$profile" "$dir"; then
    echo "install: added ${dir} to PATH in ${profile}"
    echo "install: open a new shell, or run: . ${profile}"
  else
    echo "install: warning: could not update ${profile}; add ${dir} to PATH yourself" >&2
  fi
}

linger_state() {
  if ! have_command loginctl; then
    printf 'unknown'
    return 0
  fi
  user="$(current_user)"
  if [ -z "$user" ]; then
    printf 'unknown'
    return 0
  fi
  loginctl show-user "$user" -p Linger 2>/dev/null |
    sed -n 's/^Linger=//p' || true
  return 0
}

report_linger() {
  state="$(linger_state)"
  case "$state" in
    yes|1)
      echo "install: lingering is enabled — services keep running after logout"
      return 0
      ;;
  esac
  user="$(current_user)"
  if [ "$enable_linger" -eq 1 ] && [ "$dry_run" -eq 0 ] &&
    have_command loginctl && [ -n "$user" ]; then
    if loginctl enable-linger "$user" >/dev/null 2>&1; then
      echo "install: enabled lingering for ${user}"
      return 0
    fi
    echo "install: warning: could not enable lingering without more privileges" >&2
  fi
  if [ "$dry_run" -eq 1 ] && [ "$enable_linger" -eq 1 ]; then
    echo "install: would try to enable lingering for ${user:-$USER}"
    return 0
  fi
  echo "install: tip: enable lingering so services keep running after logout:"
  echo "  sudo loginctl enable-linger ${user:-${USER:-your-user}}"
}

remove_path_block_if_present() {
  file="$1"
  [ -f "$file" ] || return 0
  grep -q "# >>> berthpanel installer: PATH >>>" "$file" || return 0
  if [ "$dry_run" -eq 1 ]; then
    echo "install: would remove the PATH block from ${file}"
    return 0
  fi
  if remove_path_block "$file"; then
    echo "install: removed the PATH block from ${file}"
  else
    echo "install: warning: could not update ${file}" >&2
  fi
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

  remove_path_block_if_present "$HOME/.profile"
  remove_path_block_if_present "$HOME/.bashrc"
  remove_path_block_if_present "$HOME/.bash_profile"
  remove_path_block_if_present "${ZDOTDIR:-$HOME}/.zshrc"
  if [ -n "$profile_file" ]; then
    remove_path_block_if_present "$profile_file"
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
  # Resolve artifacts: use local files when present, otherwise fetch from a
  # version-pinned release URL (never a floating `latest`). Precedence for the
  # base: --version, then BERTHPANEL_RELEASE_URL, then the pinned default.
  if [ -z "$release_url" ]; then
    if [ -n "$version" ]; then
      release_url="https://github.com/${REPO_SLUG}/releases/download/${version}"
    else
      release_url="$DEFAULT_RELEASE_URL"
    fi
  fi
  # BERTHPANEL_RELEASE_PUB names the trusted key before we decide what to fetch.
  if [ -z "$pub_file" ] && [ -n "${BERTHPANEL_RELEASE_PUB:-}" ]; then
    pub_file="$BERTHPANEL_RELEASE_PUB"
  fi

  work_dir="$source_dir"
  if [ ! -f "$work_dir/${BIN_NAME}" ] || [ ! -f "$work_dir/SHA256SUMS" ]; then
    work_dir="$(mktemp -d)"
    echo "install: no local artifacts in ${source_dir}; fetching from ${release_url}"
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
      pub_file="$work_dir/release.pub"
    fi
    [ -f "$pub_file" ] || die "trusted public key not found at ${pub_file}"
    minisign_bin="$(ensure_minisign)"
    echo "install: verifying SHA256SUMS against ${pub_file}"
    verify_signature "$minisign_bin" "$pub_file" \
      "$work_dir/SHA256SUMS" "$work_dir/SHA256SUMS.minisig" ||
      die "signature verification FAILED — refusing to install"
  fi

  echo "install: verifying artifact checksums"
  # SHA256SUMS also covers install.sh and the source tarball, which a binary
  # install neither fetched nor uses; check exactly the artifacts present and
  # needed. The binary's own line is mandatory — a guard that cannot fail is
  # not a guard (dev.md §11), so a missing line is loud, never a silent skip.
  filtered="$(mktemp)"
  for artifact in "$BIN_NAME" release.pub; do
    [ -f "$work_dir/${artifact}" ] || continue
    if line="$(awk -v f="$artifact" '$2 == f { print; found = 1 } END { if (!found) exit 1 }' "$work_dir/SHA256SUMS")"; then
      printf '%s\n' "$line" >> "$filtered"
    elif [ "$artifact" = "$BIN_NAME" ]; then
      rm -f "$filtered"
      die "no ${BIN_NAME} entry in SHA256SUMS — refusing to install"
    else
      echo "install: warning: no ${artifact} entry in SHA256SUMS; not checked" >&2
    fi
  done
  if ! (cd "$work_dir" && sha256sum --check --strict "$filtered"); then
    rm -f "$filtered"
    die "checksum verification FAILED — refusing to install"
  fi
  rm -f "$filtered"

  dest_dir="${prefix}/bin"
  dest="${dest_dir}/${BIN_NAME}"
  if [ "$dry_run" -eq 1 ]; then
    echo "install: would install $(cd "$work_dir" && pwd)/${BIN_NAME} -> ${dest}"
    maybe_add_to_path "$dest_dir"
    report_linger
    return 0
  fi

  mkdir -p "$dest_dir"
  tmp="${dest}.tmp.$$"
  cp "$work_dir/${BIN_NAME}" "$tmp"
  chmod 0755 "$tmp"
  mv -f "$tmp" "$dest"

  echo "install: installed ${dest}"
  check_runtime_deps
  maybe_add_to_path "$dest_dir"
  report_linger

  if [ "$run_selftest" -eq 1 ]; then
    echo "install: running ${dest} --selftest"
    if ! "$dest" --selftest; then
      die "selftest failed — the binary is installed, but your user manager needs attention (see above)"
    fi
    echo "install: selftest passed"
  else
    echo "install: run '${BIN_NAME} --selftest' after opening a new shell to confirm your user manager works"
  fi
}

case "$action" in
  install) install ;;
  uninstall) uninstall ;;
esac
