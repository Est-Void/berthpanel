# Packaging (stage 8.7)

`install.sh` is and stays the **primary** install path (dev.md §10). These
files are the distribution-native alternatives:

- `PKGBUILD` — the AUR package (builds from source, installs to `/usr/bin`);
- `nfpm.yaml` — `.deb` and `.rpm`, built from the static `make musl` artifact,
  and carrying the Corresponding Source tarball (`/usr/share/berthpanel`).

## License

Settled (dev.md §12.5): **GPL-3.0 with the Corresponding Source offered in
every release.** `scripts/release-local.sh` writes the source tarball
(`release/berthpanel-<tag>-src.tar.gz`, the tagged git tree) and signs its
checksum together with the binary's. The AUR package builds from source, so it
is compliant as-is; the deb/rpm ship the source tarball inside the package.

## Release steps

1. `scripts/keygen-release.sh`; commit `docs/release.pub`.
2. Cut a version, write `release/notes/vX.Y.Z.md`.
3. `scripts/release-local.sh --sign`; commit `release/SHA256SUMS*` with the tag.
4. In `PKGBUILD`, set `pkgver` and `sha256sums` from the release source tarball.
5. `make musl && scripts/release-local.sh` then
   `BERTHPANEL_VERSION=vX.Y.Z nfpm package -f packaging/nfpm.yaml -p deb`
   (and `-p rpm`).
