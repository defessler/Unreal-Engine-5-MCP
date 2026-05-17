#!/usr/bin/env python3
# Sort #include lines alphabetically within each group, and sort forward
# declarations alphabetically. Per the project coding-conventions:
#
#   Include paths | ... Includes must be sorted alphabetically within
#   their group.
#
#   IWYU | ... `generated.h` stays last. Forward declarations must also
#   be sorted alphabetically.
#
# Groups are separated by blank lines or other non-include lines. Within
# each group, lines are sorted case-insensitively by include path.
#
# Special rules:
#   - `*.generated.h` always sorts to the END of its group (UE convention;
#     UHT requires it to be the last #include in a header).
#   - The first #include in a .cpp file is left at position 0 if it
#     matches `<basename>.h`. The "matching header first" rule is a
#     standard IWYU recommendation that improves header self-containment
#     checks.
#
# Forward-declaration sort: contiguous runs of `class Foo;` / `struct Foo;`
# get the same treatment. Anything carrying a comment or differing in
# leading whitespace is left untouched.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]\s*(?://.*)?$')
FWDDECL_RE = re.compile(r'^\s*(class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*(?://.*)?$')


def sort_key_include(line: str) -> tuple[int, str]:
    """generated.h sorts last; everything else by path lowercase."""
    m = INCLUDE_RE.match(line)
    if not m:
        return (2, line.lower())
    path = m.group(1)
    if path.endswith('.generated.h'):
        return (1, path.lower())
    return (0, path.lower())


def sort_key_fwddecl(line: str) -> str:
    m = FWDDECL_RE.match(line)
    if not m:
        return line.lower()
    return m.group(2).lower()


def is_include(line: str) -> bool:
    return bool(INCLUDE_RE.match(line))


def is_fwddecl(line: str) -> bool:
    return bool(FWDDECL_RE.match(line))


def is_blank(line: str) -> bool:
    return line.strip() == ''


def find_runs(lines: list[str], predicate) -> list[tuple[int, int]]:
    """Returns [(start, end_exclusive)] of contiguous runs where every
    line satisfies `predicate`."""
    runs = []
    i = 0
    n = len(lines)
    while i < n:
        if predicate(lines[i]):
            j = i + 1
            while j < n and predicate(lines[j]):
                j += 1
            if j > i + 1:
                runs.append((i, j))
            i = j
        else:
            i += 1
    return runs


def process_file(path: Path, write: bool) -> int:
    with open(path, 'r', encoding='utf-8', errors='replace', newline='') as fh:
        text = fh.read()
    lines = text.splitlines(keepends=True)

    rewrites = 0

    # 1. Sort #include runs.
    include_runs = find_runs(lines, is_include)
    for start, end in include_runs:
        block = lines[start:end]
        sorted_block = sorted(block, key=sort_key_include)
        if sorted_block != block:
            # Special rule: in .cpp files, if the first line matches the
            # file's matching header (`<basename>.h`), keep it at the top.
            if path.suffix == '.cpp':
                basename_h = path.stem + '.h'
                idx = next(
                    (k for k, ln in enumerate(sorted_block)
                     if (m := INCLUDE_RE.match(ln)) and m.group(1).endswith('/' + basename_h)
                     or (m := INCLUDE_RE.match(ln)) and m.group(1) == basename_h),
                    None,
                )
                if idx is not None and idx != 0:
                    sorted_block.insert(0, sorted_block.pop(idx))
            if sorted_block != block:
                lines[start:end] = sorted_block
                rewrites += 1

    # 2. Sort forward-declaration runs.
    fwd_runs = find_runs(lines, is_fwddecl)
    for start, end in fwd_runs:
        block = lines[start:end]
        sorted_block = sorted(block, key=sort_key_fwddecl)
        if sorted_block != block:
            lines[start:end] = sorted_block
            rewrites += 1

    if write and rewrites > 0:
        with open(path, 'w', encoding='utf-8', newline='') as fh:
            fh.write(''.join(lines))

    return rewrites


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--write', action='store_true')
    args = ap.parse_args()
    total = 0
    for f in args.files:
        p = Path(f)
        if not p.exists():
            continue
        r = process_file(p, args.write)
        if r:
            print(f'{f}: {r} rewrites')
        total += r
    print(f'TOTAL: {total} rewrites')
    return 0


if __name__ == '__main__':
    sys.exit(main())
