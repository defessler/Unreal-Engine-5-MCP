#!/usr/bin/env python3
# Converts each `namespace { ... }` in a .cpp file to a named namespace
# `namespace <filebase>_detail { ... }` per the project coding-conventions:
#
#   Never use `namespace { … }` in `.cpp` files. Unreal builds with unity
#   builds (multiple .cpp files concatenated), which collapses every
#   anonymous namespace in the unity blob into a single namespace — so
#   symbols collide across files, ODR violations slip past local builds
#   and only blow up in CI/Shipping. Prefer a named namespace
#   `namespace <Project>::<Feature> { … }` for grouped helpers ...
#
# Naming rule (chosen for uniqueness in unity-build context):
#
#   <file_basename_snake_case>_detail
#
# So `Decompile.cpp` → `namespace decompile_detail`,
#    `BackendFactory.cpp` → `namespace backend_factory_detail`,
#    `main.cpp` → `namespace main_detail`.
#
# Since each .cpp file's outer namespace differs by directory (e.g.,
# `bpr::tools` vs `bpr::backends`), and the inner-detail name carries the
# file basename, the resulting FQN is unique across the codebase. This is
# exactly what unity builds need.
#
# Rewrites:
#   - `^namespace \{$`  →  `^namespace <name> {$`
#   - The matching `^}$` close gets a trailing `// namespace <name>` comment
#     (matches the closing-comments convention).
#
# Conservative:
#   - Only touches anon namespaces that open at the start of a line with
#     exactly `namespace {` (and optional trailing whitespace).
#   - Only handles a single anon namespace per file. Multiple anon
#     namespaces in the same file get separate names: `<base>_detail`,
#     `<base>_detail2`, ...

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def to_snake(name: str) -> str:
    # CamelCase → snake_case. Insert `_` before uppercase letters
    # following a lowercase or digit.
    s = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', name)
    return s.lower()


def find_matching_close(lines: list[str], open_line: int) -> int:
    """Given the line index of `namespace {`, walk forward tracking brace
    depth (depth=1 right after the `{`) and return the line index of the
    matching `}`. Strips strings/comments per line for robustness."""
    depth = 1
    for j in range(open_line + 1, len(lines)):
        s = strip_strings_and_comments_line(lines[j])
        for c in s:
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return j
    return -1


def strip_strings_and_comments_line(line: str) -> str:
    out = list(line)
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == '/' and i + 1 < n and line[i+1] == '/':
            for j in range(i, n):
                if line[j] != '\n':
                    out[j] = ' '
            break
        if c == '/' and i + 1 < n and line[i+1] == '*':
            out[i] = out[i+1] = ' '
            i += 2
            while i + 1 < n and not (line[i] == '*' and line[i+1] == '/'):
                if line[i] != '\n':
                    out[i] = ' '
                i += 1
            if i + 1 < n:
                out[i] = out[i+1] = ' '
                i += 2
            continue
        if c == '"':
            out[i] = ' '
            i += 1
            while i < n and line[i] != '"':
                if line[i] == '\\' and i + 1 < n:
                    out[i] = out[i+1] = ' '
                    i += 2
                    continue
                out[i] = ' '
                i += 1
            if i < n:
                out[i] = ' '
                i += 1
            continue
        if c == "'":
            out[i] = ' '
            i += 1
            while i < n and line[i] != "'":
                if line[i] == '\\' and i + 1 < n:
                    out[i] = out[i+1] = ' '
                    i += 2
                    continue
                out[i] = ' '
                i += 1
            if i < n:
                out[i] = ' '
                i += 1
            continue
        i += 1
    return ''.join(out)


# Multi-line block-comment state isn't carried across lines in this script;
# we accept that comments spanning lines might confuse the inner brace
# counter, but for the codebase's style (block comments are rare in .cpp
# bodies and never contain stray `{`/`}` per inspection) the risk is zero.

ANON_OPEN = re.compile(r'^namespace\s*\{\s*$')
ANON_OPEN_BRACE_NEXT = re.compile(r'^namespace\s*$')


def process_file(path: Path, write: bool) -> int:
    with open(path, 'r', encoding='utf-8', errors='replace', newline='') as fh:
        text = fh.read()
    lines = text.splitlines(keepends=True)

    base = to_snake(path.stem)
    rewrites = 0
    counter = 0

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.rstrip('\n').rstrip('\r')
        # Match: `namespace {` on one line.
        if ANON_OPEN.match(stripped):
            counter += 1
            name = f'{base}_detail' if counter == 1 else f'{base}_detail{counter}'
            # Find matching close.
            close = find_matching_close(lines, i)
            if close < 0:
                i += 1
                continue
            # Rewrite open.
            lines[i] = f'namespace {name} {{\n'
            # Rewrite close. Append a `using namespace <name>;` shim
            # immediately after so unqualified references in the rest of
            # the file continue to resolve (anonymous-namespace lookup
            # semantics).
            lines[close] = f'}}    // namespace {name}\nusing namespace {name};\n'
            rewrites += 1
            i = close + 1
            continue
        # Match: `namespace` on one line, `{` on the next (Allman).
        if ANON_OPEN_BRACE_NEXT.match(stripped):
            if i + 1 < len(lines) and lines[i+1].strip() == '{':
                counter += 1
                name = f'{base}_detail' if counter == 1 else f'{base}_detail{counter}'
                close = find_matching_close(lines, i + 1)
                if close < 0:
                    i += 1
                    continue
                lines[i] = f'namespace {name}\n'
                # Leave lines[i+1] alone (the `{` stays on its own line).
                lines[close] = f'}}    // namespace {name}\nusing namespace {name};\n'
                rewrites += 1
                i = close + 1
                continue
        i += 1

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
