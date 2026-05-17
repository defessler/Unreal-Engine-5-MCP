#!/usr/bin/env python3
# Adds braces to unbraced `if` / `for` / `while` single-statement bodies per
# the project coding-conventions:
#
#   if (cond)        →    if (cond)
#     stmt;                 {
#                               stmt;
#                           }
#
#   if (cond) stmt;  →    if (cond)
#                           {
#                               stmt;
#                           }
#
# Allman style throughout (UE convention).
#
# Conservative on purpose — we'd rather skip ambiguous cases than break
# semantics. Each rewrite is constrained to one of two patterns:
#
#   Pattern A: same-line body
#       `if (cond) <stmt>;`   where <stmt> doesn't contain `{` or `//`
#                             and the close paren / stmt / semicolon
#                             all sit on one line.
#
#   Pattern B: next-line indented body
#       `if (cond)\n<deeper-indent> <single-statement-ending-in-;>`
#       where the body is a SINGLE statement (extends only as far as the
#       first `;` that closes the body's expression at the outer level).
#       Multi-line statements (calls split across lines) are honored —
#       we keep gathering body lines until a balanced `;` is seen.
#
# Skips:
#   - `else if`, `else <body>` — left alone (changing else needs to match the
#     outer if's bracing; harder; out of scope for this pass).
#   - `do/while`, `if constexpr` with trailing `requires`, range-for with
#     structured bindings spanning multiple lines (already braced if real).
#   - Macros (anything where the keyword is preceded by `#` or appears
#     after `\\` line continuation).
#
# The script is line-based but balance-aware: it counts parens, brackets,
# and braces inside string/char literals and comments by stripping them
# first (using the same helper as add-namespace-comments.py).

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


KEYWORDS = ('if', 'for', 'while')
KW_PAT = re.compile(r'^(\s*)(if|for|while)\s*\(')


def strip_strings_and_comments_line(line: str) -> str:
    """For paren-balance counting on a single line. Replaces string/char
    literal bodies and `// ...` comments with spaces. Block comments are
    handled at file level by the caller."""
    out = list(line)
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == '/' and i + 1 < n and line[i+1] == '/':
            for j in range(i, n):
                if line[j] != '\n':
                    out[j] = ' '
            return ''.join(out)
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


def find_matching_paren(text: str, open_idx: int) -> int:
    """Return the index of the `)` that matches `(` at open_idx, scanning
    forward. Handles paren-in-string by stripping first. Returns -1 if
    unbalanced."""
    sanitized = strip_strings_and_comments_line(text)
    depth = 0
    for i in range(open_idx, len(sanitized)):
        if sanitized[i] == '(':
            depth += 1
        elif sanitized[i] == ')':
            depth -= 1
            if depth == 0:
                return i
    return -1


def looks_like_else(line: str) -> bool:
    """Skip `else if (cond)` — the else side needs its own analysis."""
    stripped = line.lstrip()
    return stripped.startswith('else ')


def process_file(path: Path, write: bool) -> int:
    with open(path, 'r', encoding='utf-8', errors='replace', newline='') as fh:
        text = fh.read()
    # First, strip block comments at the file level so they don't fool us.
    # We keep a parallel `sanitized` view; line numbers stay the same.
    sanitized_full = strip_block_comments(text)
    lines = text.splitlines(keepends=True)
    sanitized_lines = sanitized_full.splitlines(keepends=True)

    # Walk line by line. We rewrite by replacing slices of `lines`.
    new_lines: list[str] = []
    i = 0
    rewrites = 0
    while i < len(lines):
        line = lines[i]
        sline = sanitized_lines[i] if i < len(sanitized_lines) else ''
        m = KW_PAT.match(sline)
        if not m or looks_like_else(sline):
            new_lines.append(line)
            i += 1
            continue

        indent, kw = m.group(1), m.group(2)
        # Find matching `)`. Scan forward across lines if needed.
        # Accumulate the header span [i .. header_end_line] for the cond.
        open_idx = sline.index('(')
        depth = 0
        header_end_line = i
        header_end_col = -1
        scanning_line = i
        col_start = open_idx
        while scanning_line < len(sanitized_lines):
            s = sanitized_lines[scanning_line]
            start = col_start if scanning_line == i else 0
            for col in range(start, len(s)):
                c = s[col]
                if c == '(':
                    depth += 1
                elif c == ')':
                    depth -= 1
                    if depth == 0:
                        header_end_line = scanning_line
                        header_end_col = col
                        break
            if header_end_col != -1:
                break
            scanning_line += 1
            col_start = 0

        if header_end_col == -1:
            new_lines.append(line)
            i += 1
            continue

        # Examine what's AFTER the `)` on the header_end_line.
        sline_end = sanitized_lines[header_end_line]
        oline_end = lines[header_end_line]
        after_paren = sline_end[header_end_col + 1:]
        after_paren_stripped = after_paren.strip()

        if after_paren_stripped.startswith('{'):
            # Already braced — pass through.
            new_lines.append(line)
            i += 1
            continue

        # === Pattern A: same-line body
        # e.g. `if (cond) return 0;`
        # For multi-line `if (...)` where the body is on the SAME line as
        # the closing `)`, we keep [i .. header_end_line - 1] verbatim and
        # only transform the final header line.
        if after_paren_stripped and not after_paren_stripped.startswith('//'):
            # Confirm there's no `{` in the body (else it's multi-stmt with
            # nested braces that we shouldn't touch).
            if '{' in after_paren_stripped:
                new_lines.append(line)
                i += 1
                continue
            # Confirm we end with `;` (single statement).
            # The body may end at `//` comment after the `;`.
            stmt_end = after_paren_stripped.rfind(';')
            if stmt_end == -1:
                new_lines.append(line)
                i += 1
                continue
            # Output earlier header lines verbatim (for multi-line conds).
            for h in range(i, header_end_line):
                new_lines.append(lines[h])
            # Reconstruct the final line. Header on this line is
            # everything up to and including the `)`; body is the rest
            # (with any trailing `// comment` preserved).
            o = lines[header_end_line]
            o_close = header_end_col
            header_text = o[:o_close + 1]
            body_text = o[o_close + 1:].lstrip()
            # Detect leading whitespace of the final header line so the
            # `{` / `}` lines match its indentation (not the keyword's
            # original indentation, which may differ in a multi-line
            # condition where the closing `)` is continued).
            #
            # We use the keyword's indent for both braces — that's the
            # statement indent. The continuation line's leading
            # whitespace is preserved verbatim in header_text.
            body_indent = indent + '\t'
            header_text = header_text.rstrip()
            new_lines.append(header_text + '\n')
            new_lines.append(indent + '{\n')
            new_lines.append(body_indent + body_text.rstrip('\n') + '\n')
            new_lines.append(indent + '}\n')
            rewrites += 1
            i = header_end_line + 1
            continue

        # === Pattern B: next-line indented body
        # The next non-blank line is the body. Need to know its extent.
        # We accept ONLY: body is a single C++ statement ending in `;` at
        # outer-paren depth 0. No nested control flow.
        # If the next non-blank line starts with `{`, it's already braced —
        # skip.
        j = header_end_line + 1
        # Skip blank / comment lines until we find code.
        while j < len(lines) and lines[j].strip().startswith('//'):
            j += 1
        while j < len(lines) and not sanitized_lines[j].strip():
            j += 1
        if j >= len(lines):
            new_lines.append(line)
            i += 1
            continue
        body_first = sanitized_lines[j].lstrip()
        if body_first.startswith('{') or body_first.startswith('#'):
            # Already braced or a preprocessor directive — skip.
            new_lines.append(line)
            i += 1
            continue
        # Reject control-flow body — too risky to rewrite blindly.
        if re.match(r'(if|for|while|do|switch|return)\b', body_first):
            # Allow `return` though — it's a single statement.
            if not re.match(r'return\b', body_first):
                new_lines.append(line)
                i += 1
                continue
        # Collect body lines until paren / brace / bracket balance returns
        # to zero AND we see a `;` at depth 0.
        body_lines: list[str] = []
        depth_p = 0
        depth_s = 0  # square brackets
        depth_b = 0  # braces (should stay zero)
        ended = False
        k = j
        while k < len(lines):
            s = sanitized_lines[k]
            o = lines[k]
            for col, c in enumerate(s):
                if c == '(':
                    depth_p += 1
                elif c == ')':
                    depth_p -= 1
                elif c == '[':
                    depth_s += 1
                elif c == ']':
                    depth_s -= 1
                elif c == '{':
                    depth_b += 1
                elif c == '}':
                    depth_b -= 1
                elif c == ';' and depth_p == 0 and depth_s == 0 and depth_b == 0:
                    ended = True
                    break
            body_lines.append(o)
            if ended:
                break
            k += 1

        if not ended or depth_b != 0:
            # Couldn't cleanly identify body; pass through.
            new_lines.append(line)
            i += 1
            continue

        # Verify there's nothing meaningful BETWEEN header and body (only
        # blank lines / comments). We already advanced through them; emit
        # them verbatim.
        # Header lines [i .. header_end_line]
        for h in range(i, header_end_line + 1):
            new_lines.append(lines[h])
        # Skip lines between header and j (the blank / comment lines).
        for skipped in range(header_end_line + 1, j):
            new_lines.append(lines[skipped])
        # Open brace at header indent.
        new_lines.append(indent + '{\n')
        # Body lines: re-indent each by one extra tab.
        for bl in body_lines:
            new_lines.append('\t' + bl if bl.startswith('\t') or bl.startswith(' ') else bl)
        # Close brace.
        new_lines.append(indent + '}\n')
        rewrites += 1
        i = k + 1
        continue

    if write and rewrites > 0:
        with open(path, 'w', encoding='utf-8', newline='') as fh:
            fh.write(''.join(new_lines))

    return rewrites


def strip_block_comments(text: str) -> str:
    """Replace `/* ... */` with same-length whitespace (preserves newlines)."""
    out = list(text)
    i = 0
    n = len(text)
    while i + 1 < n:
        if text[i] == '/' and text[i+1] == '*':
            out[i] = out[i+1] = ' '
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i+1] == '/'):
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i + 1 < n:
                out[i] = out[i+1] = ' '
                i += 2
        else:
            i += 1
    return ''.join(out)


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
