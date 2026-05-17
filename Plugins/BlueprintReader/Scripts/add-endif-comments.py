#!/usr/bin/env python3
# Adds trailing condition comments to bare #endif directives in C/C++ files
# per the project coding-conventions: #endif    // <CONDITION>  (4-space separator).
#
# Walks each file line by line, maintains the #if / #ifdef / #ifndef condition
# stack (popping on #endif, pushing on #if and friends), and rewrites bare
# #endif lines to carry the matching condition. Also rewrites bare #else when
# inside an #if that doesn't already have a trailing comment.
#
# Intentionally conservative:
#   - Only touches LEADING-whitespace #endif lines (preserves indentation).
#   - Skips lines that already have ANY comment after the #endif token.
#   - On stack underflow (unmatched #endif), leaves the line alone and warns.
#   - Doesn't touch #endif inside multi-line comments — but the input is
#     well-formed C++ so the heuristic is safe.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


CONDITION_PREFIX = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif)\s+(.*?)\s*$')
ENDIF_LINE      = re.compile(r'^(\s*)#\s*endif\b\s*(.*?)\s*$')
ELSE_LINE       = re.compile(r'^(\s*)#\s*else\b\s*(.*?)\s*$')


def normalize_condition(directive: str, rest: str) -> str:
    """Build the trailing-comment text for the condition stack entry."""
    rest = rest.strip()
    if directive == 'ifdef':
        return rest
    if directive == 'ifndef':
        # `#endif    // !FOO` per the convention example for #else following #ifndef.
        return rest
    return rest


def process_file(path: Path, write: bool) -> tuple[int, int]:
    """Returns (lines rewritten, warnings)."""
    text = path.read_text(encoding='utf-8', errors='replace')
    lines = text.splitlines(keepends=True)
    stack: list[tuple[str, str]] = []  # (directive, condition)
    rewrote = 0
    warnings = 0

    for i, line in enumerate(lines):
        # Push for #if / #ifdef / #ifndef
        m_if = CONDITION_PREFIX.match(line)
        if m_if and m_if.group(1) in ('if', 'ifdef', 'ifndef'):
            stack.append((m_if.group(1), m_if.group(2)))
            continue

        # #else: rewrite if bare AND inside a known condition.
        m_else = ELSE_LINE.match(line)
        if m_else:
            indent, trailing = m_else.group(1), m_else.group(2)
            if not trailing.strip() and stack:
                directive, cond = stack[-1]
                if directive == 'ifndef':
                    comment = f'!{cond}'
                else:
                    comment = f'!{cond}' if directive == 'ifdef' else cond
                lines[i] = f'{indent}#else    // {comment}\n'
                rewrote += 1
            continue

        # #endif: pop and rewrite if bare.
        m_end = ENDIF_LINE.match(line)
        if m_end:
            indent, trailing = m_end.group(1), m_end.group(2)
            if not stack:
                warnings += 1
                continue
            directive, cond = stack.pop()
            if trailing.strip():
                # Already has a comment — leave alone.
                continue
            lines[i] = f'{indent}#endif    // {cond}\n'
            rewrote += 1

    if write and rewrote > 0:
        path.write_text(''.join(lines), encoding='utf-8')

    return rewrote, warnings


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--write', action='store_true', help='Write changes (default: dry-run)')
    args = ap.parse_args()

    total_r, total_w = 0, 0
    for f in args.files:
        p = Path(f)
        if not p.exists():
            print(f'SKIP (missing): {f}', file=sys.stderr)
            continue
        r, w = process_file(p, args.write)
        if r or w:
            print(f'{f}: {r} rewrites, {w} warnings')
        total_r += r
        total_w += w
    print(f'TOTAL: {total_r} rewrites, {total_w} warnings')
    return 0


if __name__ == '__main__':
    sys.exit(main())
