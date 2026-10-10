#!/usr/bin/env bash
#
# scripts/keygen-release.sh — one-time release key generation (dev.md §10).
#
# Creates the maintainer's minisign keypair under the XDG config dir:
#
#   ${XDG_CONFIG_HOME:-$HOME/.config}/berthpanel/release.key   0600  SECRET
#   ${XDG_CONFIG_HOME:-$HOME/.config}/berthpanel/release.pub   0644  public
#
# prints the key fingerprint, and reminds you to commit ONLY the public half
# (as docs/release.pub) and paste the fingerprint into the README trust
# section. The secret key NEVER enters the repository; CI only ever verifies.
#
# Overridable: MINISIGN (path to the minisign binary), BERTHPANEL_RELEASE_KEY_DIR.
# Refuses to overwrite an existing key unless --force — fail closed, so a
# mistyped re-run cannot destroy the key a release was signed with.

set -euo pipefail

force=0
case "${1:-}" in
  --force) force=1 ;;
  "") ;;
  *)
    echo "usage: scripts/keygen-release.sh [--force]" >&2
    exit 2
    ;;
esac

minisign_bin="${MINISIGN:-minisign}"
if ! command -v "${minisign_bin}" >/dev/null 2>&1; then
  echo "keygen: minisign is required and was not found on PATH" >&2
  echo "        install it first — keygen never downloads a signing tool" >&2
  exit 1
fi

config_dir="${BERTHPANEL_RELEASE_KEY_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/berthpanel}"
key="${config_dir}/release.key"
pub="${config_dir}/release.pub"

mkdir -p "${config_dir}"
chmod 0700 "${config_dir}"

if [ -e "${key}" ] && [ "${force}" -ne 1 ]; then
  echo "keygen: refusing to overwrite ${key} (pass --force to regenerate)" >&2
  exit 1
fi

echo "keygen: generating a minisign keypair in ${config_dir}"
# -G generates; minisign prompts for the passphrase on the terminal and never
# stores it. There is no passphrase in argv or in the environment.
"${minisign_bin}" -G -p "${pub}" -s "${key}"

chmod 0600 "${key}"
chmod 0644 "${pub}"

# The fingerprint is the key id carried in the public file's untrusted comment.
fingerprint="$(head -n1 "${pub}" | sed -e 's/^untrusted comment: minisign public key: //')"

echo
echo "keygen: public key:  ${pub}"
echo "keygen: fingerprint: ${fingerprint}"
echo
echo "Next steps, by hand:"
echo "  1. cp '${pub}' docs/release.pub     # the PUBLIC half is committed"
echo "  2. put the fingerprint in the README trust section"
echo "  3. never commit '${key}'"
