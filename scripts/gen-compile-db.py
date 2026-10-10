#!/usr/bin/env python3
"""Generate a compile_commands.json for `make check` (clang-tidy).

The source list and the compile flags come from the Makefile, so there is a
single source of truth and this script only serializes them. It writes nothing
but the requested --out file (under build/, which is gitignored): `make check`
must never leave a change in the tree (dev.md §3).
"""

import argparse
import json
import os
import shlex


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--flags", required=True)
    parser.add_argument("--cwd", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()

    flags = shlex.split(args.flags)
    entries = []
    for rel in args.files:
        entries.append(
            {
                "directory": args.cwd,
                "file": os.path.join(args.cwd, rel),
                "arguments": [args.cxx, *flags, "-c", rel, "-o", os.devnull],
            }
        )
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(entries, handle, indent=1)
        handle.write("\n")


if __name__ == "__main__":
    main()
