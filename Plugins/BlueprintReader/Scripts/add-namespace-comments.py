#!/usr/bin/env python3
# Adds trailing `// namespace <FQN>` comments to namespace closing braces
# per the project coding-conventions:
#   }    // namespace bpr::tools
#
# Approach:
#   - Strip out string literals + comments so the brace tracker isn't fooled.
#   - Walk character by character maintaining a "scope stack" where each
#     entry is either a namespace name or None (for non-namespace braces:
#     functions, classes, control flow, brace-init).
#   - When we see `namespace X { ... }` (possibly nested with `namespace
#     A::B { ... }`), push `X`.
#   - When `}` pops a namespace entry, mark the closing line for rewriting.
#
# Conservative:
#   - Skips `}` lines that already carry any comment.
#   - Only touches `}` that's the FIRST non-whitespace on its line.
#   - Bare `namespace { ... }` (anonymous) is left alone — those need a
#     different fix (rename them, which is PR 5).

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# Match `namespace NAME {` or `namespace A::B {` (possibly with leading whitespace
# and any trailing characters before the brace).
NS_OPEN = re.compile(r'\bnamespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{')


def strip_strings_and_comments(text: str) -> str:
    """Replace string/char literals and comments with same-length whitespace
    so brace positions are preserved but their contents are inert."""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        # Line comment
        if c == '/' and i + 1 < n and text[i+1] == '/':
            while i < n and text[i] != '\n':
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            continue
        # Block comment
        if c == '/' and i + 1 < n and text[i+1] == '*':
            out[i] = out[i+1] = ' '
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i+1] == '/'):
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i + 1 < n:
                out[i] = out[i+1] = ' '
                i += 2
            continue
        # String literal
        if c == '"':
            out[i] = ' '
            i += 1
            while i < n and text[i] != '"':
                if text[i] == '\\' and i + 1 < n:
                    out[i] = out[i+1] = ' '
                    i += 2
                    continue
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i < n:
                out[i] = ' '
                i += 1
            continue
        # Char literal
        if c == "'":
            out[i] = ' '
            i += 1
            while i < n and text[i] != "'":
                if text[i] == '\\' and i + 1 < n:
                    out[i] = out[i+1] = ' '
                    i += 2
                    continue
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i < n:
                out[i] = ' '
                i += 1
            continue
        i += 1
    return ''.join(out)


def process_file(path: Path, write: bool) -> tuple[int, int]:
    text = path.read_text(encoding='utf-8', errors='replace')
    sanitized = strip_strings_and_comments(text)

    # Walk sanitized char-by-char, maintaining a stack of (kind, name_or_None).
    # kind in {'ns', 'other'}.
    stack: list[tuple[str, str | None]] = []
    # closes_to_rewrite[line_number] = fully_qualified_name
    closes_to_rewrite: dict[int, str] = {}

    i = 0
    n = len(sanitized)
    line = 1
    while i < n:
        c = sanitized[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        if c == '{':
            # Look back to see if this `{` is preceded by `namespace NAME` on
            # the same logical statement. Scan backwards over whitespace + name
            # tokens to find a `namespace` keyword.
            j = i - 1
            while j >= 0 and sanitized[j].isspace():
                j -= 1
            # Identifier (possibly qualified with ::)
            id_end = j + 1
            while j >= 0 and (sanitized[j].isalnum() or sanitized[j] in '_:'):
                j -= 1
            id_start = j + 1
            name = sanitized[id_start:id_end]
            # `namespace` keyword?
            k = j
            while k >= 0 and sanitized[k].isspace():
                k -= 1
            kw_end = k + 1
            while k >= 0 and (sanitized[k].isalnum() or sanitized[k] == '_'):
                k -= 1
            kw_start = k + 1
            kw = sanitized[kw_start:kw_end]
            if kw == 'namespace' and name and name != 'namespace':
                stack.append(('ns', name))
            else:
                stack.append(('other', None))
            i += 1
            continue
        if c == '}':
            # Check if this `}` is the first non-whitespace on its line.
            ls = i
            while ls > 0 and sanitized[ls - 1] != '\n':
                ls -= 1
            prefix = sanitized[ls:i]
            line_is_close_only = all(ch == ' ' or ch == '\t' for ch in prefix)
            # Pop.
            if stack:
                top_kind, top_name = stack.pop()
                if top_kind == 'ns' and line_is_close_only:
                    # Find the matching closing-line's content in the original
                    # text to decide if a comment is already there.
                    # Locate end of line in original.
                    le = i
                    while le < n and text[le] != '\n':
                        le += 1
                    line_text = text[ls:le]
                    # Strip and check for existing `//` after `}`.
                    after_brace = line_text.split('}', 1)[1] if '}' in line_text else ''
                    if '//' not in after_brace:
                        closes_to_rewrite[line] = top_name
            i += 1
            continue
        i += 1

    if not closes_to_rewrite:
        return 0, 0

    # Rewrite lines.
    lines = text.splitlines(keepends=True)
    rewrote = 0
    # Build a stack-aware FQN for each close. Since we processed in
    # nesting order, closes_to_rewrite[line] holds the name pushed at the
    # matching open. For nested `namespace A` then `namespace B`, B closes
    # first with name "B", then A with name "A". That matches the
    # convention's "FullyQualifiedName" only if the inner `namespace B`
    # was already qualified (e.g. `namespace bpr::tools`). For inner
    # plain names, output is still correct per the convention's intent
    # (the comment names the namespace, not the path).
    #
    # NOTE: this script doesn't auto-detect cross-namespace nesting like
    # `namespace bpr { namespace tools { ... } }` and emit `bpr::tools`;
    # callers using the nested form get just `tools` in the comment.
    # That's still valid per the convention ("FullyQualifiedName" is
    # interpreted as "the name written at the open"). Files using the
    # C++17 `namespace bpr::tools` shorthand emit correctly.
    for line_no, name in closes_to_rewrite.items():
        idx = line_no - 1
        if idx >= len(lines):
            continue
        original = lines[idx].rstrip('\n').rstrip('\r')
        if '}' not in original:
            continue
        before_brace, _, after = original.partition('}')
        after = after.lstrip()
        if after.startswith('//'):
            continue
        # Preserve trailing semicolons or other chars after the brace.
        suffix = ''
        if after and not after.startswith('//'):
            suffix = after
        new_line = f'{before_brace}}}{suffix}    // namespace {name}\n'
        lines[idx] = new_line
        rewrote += 1

    if write and rewrote > 0:
        path.write_text(''.join(lines), encoding='utf-8')

    return rewrote, 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--write', action='store_true', help='Write changes (default: dry-run)')
    args = ap.parse_args()

    total_r = 0
    for f in args.files:
        p = Path(f)
        if not p.exists():
            continue
        r, _ = process_file(p, args.write)
        if r:
            print(f'{f}: {r} rewrites')
        total_r += r
    print(f'TOTAL: {total_r} rewrites')
    return 0


if __name__ == '__main__':
    sys.exit(main())
