#!/bin/sh
#
# scripts/musl-inner.sh — runs INSIDE the pinned Arch container (stage 8.2).
#
# Root and throwaway. It installs the pinned package set, copies the read-only
# source into a writable work dir, builds the release artifact statically
# against musl, and writes those bytes to stdout. Every diagnostic goes to
# stderr, because the caller reads stdout as the artifact itself.
#
# Nothing here installs anything on the host, and the container mounts the
# source read-only: the host tree is untouched.

set -eu

: "${MUSL_PACKAGES:=base-devel musl zlib}"
: "${MUSL_CC:=musl-gcc}"
: "${MUSL_CXX:=musl-g++}"
: "${SOURCE_DATE_EPOCH:=0}"
export SOURCE_DATE_EPOCH

log() { echo "musl[container]: $*" >&2; }

log "installing: ${MUSL_PACKAGES}"
# A fresh Arch image can have an unpopulated keyring; the retry is bounded and
# explained, never a silent fallback (dev.md §11).
# shellcheck disable=SC2086  # MUSL_PACKAGES is a controlled space-separated list.
if ! pacman -Sy --noconfirm --needed ${MUSL_PACKAGES} >&2; then
  log "pacman failed; initializing the keyring and retrying once"
  pacman-key --init >&2
  pacman-key --populate archlinux >&2
  # shellcheck disable=SC2086
  pacman -Sy --noconfirm --needed ${MUSL_PACKAGES} >&2
fi

# Arch's `musl` package ships musl-gcc (C only). C++ drives g++ through the
# same musl specs; -static pulls in the static libstdc++/libgcc so the artifact
# has no runtime dependencies. If a distro's static libstdc++ does not link
# against musl, supply a musl-native toolchain via MUSL_CXX (dev.md §12) — the
# build fails loudly rather than falling back to a dynamic binary.
if [ "${MUSL_CXX}" = "musl-g++" ] && ! command -v musl-g++ >/dev/null 2>&1; then
  specs=/usr/lib/musl/lib/musl-gcc.specs
  {
    echo '#!/bin/sh'
    echo "exec g++ -specs=${specs} \"\$@\""
  } > /usr/local/bin/musl-g++
  chmod 0755 /usr/local/bin/musl-g++
fi

rm -rf /work
cp -a /src /work
cd /work

jobs="$(nproc 2>/dev/null || echo 2)"
log "building release: CC=${MUSL_CC} CXX=${MUSL_CXX} jobs=${jobs}"

# -static is the whole point: no libc, no libstdc++, no libgcc at runtime.
# The Makefile keeps the reproducibility flags; the container only supplies the
# toolchain and the sysroot.
make -j"${jobs}" release \
  CC="${MUSL_CC}" \
  CXX="${MUSL_CXX}" \
  LDLIBS="-static -lz -pthread" 1>&2

log "emitting artifact on stdout"
cat build/release/berthpanel
