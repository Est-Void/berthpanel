#!/usr/bin/env python3
"""Comment budget gate for `make check` (AGENTS.md, Clean code rule).

A comment must state something the code cannot: an invariant, a decision
nobody can infer, a trap. Everything else is noise that rots.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

MAX_FILE_COMMENT_LINES = 48
MAX_CONSECUTIVE_RUN = 8
# 11.2% before the clean-code pass, 5.4% after. The residue is the §6/§11
# invariants; 6% is the ratchet, not a target to grow into.
MAX_TOTAL_RATIO = 0.06
MAX_RUN_WIDTH = 80

RE_COMMENT_START = re.compile(r"^\s*(//|/\*|\*)")
RE_COMMENT_CONT = re.compile(r"^\s*(\*|\*/)")
RE_FILENAME_ECHO = re.compile(r"^\s*//\s*(\S+\.(cpp|h|hpp|cc))\s*(—|--|-|:)")
RE_TODO = re.compile(r"\b(TODO|FIXME|XXX|HACK|NOTE)\b")
# Only unambiguous shapes: a stray brace, a preprocessor line, a one-statement
# line. Prose that merely starts with `word(` is a comment, not dead code.
RE_COMMENTED_CODE = re.compile(
    r"^\s*//\s*(?:\}|\{|\)|\]|;)\s*;?\s*$"
    r"|^\s*//\s*#\s*(?:include|define|pragma|ifdef|ifndef|endif|else)\b"
    r"|^\s*//\s*(?:return|delete|throw|break|continue|goto|co_return)\b.*;\s*$"
    r"|^\s*//\s*[A-Za-z_]\w*(?:::\w+)*\s*(?:\([^()]*\))?\s*;\s*$"
)
RE_BANNER = re.compile(r"^\s*//\s*[-=*_]{4,}\s*[-=*_]*\s*$")
RE_RAW = re.compile(r'(?:u8|u|U|L)?R"([^()\\ ]{0,16})\(')
RE_CHAR = re.compile(r"'(?:\\.|[^\\'])'")


def comment_lines(lines: list[str]) -> list[int]:
    """Line numbers carrying a real comment. String, raw-string and char
    literals are skipped, so a string body like `*out += "MemoryMax=1G";` is
    not a comment, and `64'000` is not a char literal."""
    out: list[int] = []
    block = False
    raw = None  # open raw-string delimiter
    carry = False  # string continued onto the next line by a trailing backslash
    for i, line in enumerate(lines, 1):
        if block:
            out.append(i)
            if "*/" in line:
                block = False
            continue
        j, n = 0, len(line)
        while j < n:
            if raw is not None:
                end = line.find(f"){raw}\"", j)
                if end < 0:
                    break
                j = end + len(raw) + 2
                raw = None
                continue
            if carry:
                if line[j] == "\\":
                    j += 2
                    continue
                carry = False
                if line[j] == '"':
                    j += 1
                continue
            m = RE_RAW.match(line, j)
            if m:
                raw = m.group(1)
                j = m.end()
                continue
            if RE_CHAR.match(line, j):
                j += 2
                continue
            c = line[j]
            if c == '"':
                k, closed = j + 1, False
                while k < n:
                    if line[k] == "\\":
                        k += 2
                        continue
                    if line[k] == '"':
                        k += 1
                        closed = True
                        break
                    k += 1
                if not closed:
                    carry = line.endswith("\\")
                    break
                j = k
                continue
            if line.startswith("//", j):
                out.append(i)
                break
            if line.startswith("/*", j):
                out.append(i)
                block = line.find("*/", j) < 0
                break
            j += 1
    return sorted(set(out))


def check_file(path: Path, rel: str) -> list[str]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    problems: list[str] = []
    hits = set(comment_lines(lines))

    if len(hits) > MAX_FILE_COMMENT_LINES:
        problems.append(f"{rel}: {len(hits)} comment lines (cap {MAX_FILE_COMMENT_LINES})")

    run = 0
    for i in range(1, len(lines) + 1):
        if i in hits:
            run += 1
            if run > MAX_CONSECUTIVE_RUN and not RE_TODO.search(lines[i - 1]):
                problems.append(
                    f"{rel}:{i}: comment run longer than {MAX_CONSECUTIVE_RUN} lines"
                )
                run = 0
        else:
            run = 0

    for i in hits:
        line = lines[i - 1]
        if len(line) > MAX_RUN_WIDTH:
            problems.append(f"{rel}:{i}: comment line exceeds {MAX_RUN_WIDTH} columns")
        if RE_BANNER.match(line):
            problems.append(f"{rel}:{i}: decorative banner comment")
        if RE_FILENAME_ECHO.match(line):
            problems.append(f"{rel}:{i}: file-echo header (restates the filename)")
        if RE_COMMENTED_CODE.match(line) and not RE_TODO.search(line):
            problems.append(f"{rel}:{i}: commented-out code")

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="*", default=["src", "tests"])
    ap.add_argument("--max-ratio", type=float, default=MAX_TOTAL_RATIO)
    args = ap.parse_args()

    roots = [Path(p) for p in (args.paths or ["src", "tests"])]
    files = sorted(
        p for root in roots for p in root.rglob("*") if p.suffix in (".cpp", ".h", ".hpp", ".cc")
    )
    if not files:
        print(f"comment-lint: no sources found under {', '.join(str(r) for r in roots)}", file=sys.stderr)
        return 1

    problems: list[str] = []
    total_comments = 0
    total_lines = 0
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        total_lines += len(lines)
        total_comments += len(comment_lines(lines))
        problems += check_file(path, str(path))

    ratio = total_comments / total_lines if total_lines else 0.0
    if ratio > args.max_ratio:
        problems.append(
            f"repo: comment ratio {ratio:.1%} exceeds cap {args.max_ratio:.1%} "
            f"({total_comments}/{total_lines})"
        )

    if problems:
        for p in problems:
            print(f"comment-lint: {p}", file=sys.stderr)
        print(f"comment-lint: {len(problems)} violation(s)", file=sys.stderr)
        return 1

    print(
        f"comment-lint: ok ({len(files)} files, "
        f"{total_comments}/{total_lines} comment lines = {ratio:.1%})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())