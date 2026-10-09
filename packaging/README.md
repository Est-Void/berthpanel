# Packaging (stage 8.7) — prepared, blocked on the license decision

`install.sh` is and stays the **primary** install path (dev.md §10). These
files are the distribution-native alternatives:

- `PKGBUILD` — the AUR package (builds from source, installs to `/usr/bin`);
- `nfpm.yaml` — `.deb` and `.rpm`, built from the static `make musl` artifact.

## Blocker

Both are **drafts and must not be published** until §12.5 is resolved (stage
8.6). The repository is GPL-3.0, so distributing a binary without offering the
Corresponding Source is not allowed. The proposal under consideration — stay
GPL-3.0 and ship a source tarball in each release — makes these packages legal;
until the maintainer confirms it, packaging stops here.

## To finalize once §12.5 is settled

1. `scripts/keygen-release.sh`; commit `docs/release.pub`.
2. Cut a version, write `release/notes/vX.Y.Z.md`.
3. `scripts/release-local.sh --sign`; commit `release/SHA256SUMS*` with the tag.
4. In `PKGBUILD`, set `pkgver` and the `sha256sums` of the release source
   tarball, then drop the `BLOCKED` note.
5. `make musl && BERTHPANEL_VERSION=vX.Y.Z nfpm package -f packaging/nfpm.yaml -p deb`
   (and `-p rpm`).
