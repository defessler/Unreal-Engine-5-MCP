#!/usr/bin/env python3
# Wraps `if (Ptr)` / `if (!Ptr)` patterns with `IsValid(Ptr)` /
# `!IsValid(Ptr)` when `Ptr` is locally declared as a UObject-hierarchy
# pointer. Conservative: only triggers on UNAMBIGUOUS declaration shapes.
#
# Declaration shapes recognized as UObject pointers:
#   * `U<Class>* Name = ...`                  (UE convention prefix U)
#   * `A<Class>* Name = ...`                  (UE convention prefix A)
#   * `UObject* Name = ...`
#   * `UClass* Name = ...`
#   * `auto* Name = Cast<U...|A...>(...)`     (Cast<> result is UObject)
#   * `auto* Name = NewObject<...>(...)`
#   * `<Type>* Name = SomeFn(args)->GetXxx()` only when Type starts U/A
#
# Declaration shapes EXPLICITLY skipped:
#   * `TWeakObjectPtr<T> Name` — .Get() into a local is the canonical
#     pattern, NOT IsValid() wrapping (per the project's coding
#     conventions and the user's correction in feedback memory).
#   * `FSocket* Name`, `ISocketSubsystem* Name` — non-UObject pointers.
#   * `auto Name = ...` (no `*`) — ambiguous; skip.
#   * `bool Name`, `int32 Name`, `FString Name` — non-pointer.
#
# Same-file scope check: for each declaration found, look for `if (Name)`
# /`if (!Name)` patterns within 60 lines AFTER the declaration. 60 lines
# is a heuristic for "same function scope" — gives good coverage of
# typical function lengths without crossing function boundaries usually.
#
# Skips lines that already contain `IsValid(` to avoid double-wrapping.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# Matches `(U|A)<Class>* Name = ...` or `auto* Name = Cast<U|A...>(...)`
# or `auto* Name = NewObject<...>(...)`. Captures the variable name.
# Types whose `U`-prefix matches the regex but are NOT actually UObject-
# hierarchy. UEdGraphPin is the big one in UE5+ — it's a plain struct-like
# class, not a UObject. IsValid() doesn't apply (and won't even compile
# because the overload is constrained to UObject*).
NON_UOBJECT_U_TYPES = {
    'UEdGraphPin',
}


def _build_decl_pattern_u_class():
    # `U<Class>*` but excluding NON_UOBJECT_U_TYPES.
    return re.compile(r'^\s*(?:const\s+)?(?:U[A-Z][A-Za-z0-9_]*|UObject|UClass)\s*\*\s*([A-Z][A-Za-z0-9_]*)\s*=')


DECL_PATTERNS = [
    # `U<Class>* Name = ...` (filtered post-match against NON_UOBJECT_U_TYPES)
    re.compile(r'^\s*(?:const\s+)?(U[A-Z][A-Za-z0-9_]*|UObject|UClass)\s*\*\s*([A-Z][A-Za-z0-9_]*)\s*='),
    # `A<Class>* Name = ...`
    re.compile(r'^\s*(?:const\s+)?(A[A-Z][A-Za-z0-9_]*)\s*\*\s*([A-Z][A-Za-z0-9_]*)\s*='),
    # `auto* Name = Cast<U|A...>(...)`
    re.compile(r'^\s*(?:const\s+)?auto\s*\*\s*([A-Z][A-Za-z0-9_]*)\s*=\s*Cast<[UA][A-Z]'),
    # `auto* Name = NewObject<...>(...)`
    re.compile(r'^\s*(?:const\s+)?auto\s*\*\s*([A-Z][A-Za-z0-9_]*)\s*=\s*NewObject<'),
]

IF_TRUTHY = re.compile(r'^(\s*)if\s*\(\s*([A-Z][A-Za-z0-9_]*)\s*\)(.*)$')
IF_FALSY  = re.compile(r'^(\s*)if\s*\(\s*!\s*([A-Z][A-Za-z0-9_]*)\s*\)(.*)$')


def process_file(path: Path, write: bool) -> int:
    with open(path, 'r', encoding='utf-8', errors='replace', newline='') as fh:
        text = fh.read()
    lines = text.splitlines(keepends=True)

    # Step 1: collect (line_number, var_name) for every UObject-style decl.
    decl_sites: list[tuple[int, str]] = []
    for i, line in enumerate(lines):
        for pat_idx, pat in enumerate(DECL_PATTERNS):
            m = pat.match(line)
            if not m:
                continue
            # Patterns 0 + 1 capture the type in group(1) and the
            # variable name in group(2). Patterns 2 + 3 only capture
            # the variable name in group(1) (no type to filter).
            if pat_idx in (0, 1):
                ty = m.group(1)
                name = m.group(2)
                if ty in NON_UOBJECT_U_TYPES:
                    break
            else:
                name = m.group(1)
            decl_sites.append((i, name))
            break

    if not decl_sites:
        return 0

    # Step 2: build per-name lookup of declaration line numbers (so we
    # can check "is this `if (Name)` inside the scope of an earlier decl?").
    decl_by_name: dict[str, list[int]] = {}
    for line_no, name in decl_sites:
        decl_by_name.setdefault(name, []).append(line_no)

    rewrites = 0
    for i, line in enumerate(lines):
        # Skip lines that already use IsValid.
        if 'IsValid(' in line:
            continue

        for pat, build in (
            (IF_TRUTHY,
             lambda indent, name, tail: f'{indent}if (IsValid({name})){tail}\n'),
            (IF_FALSY,
             lambda indent, name, tail: f'{indent}if (!IsValid({name})){tail}\n'),
        ):
            m = pat.match(line.rstrip('\r\n'))
            if not m:
                continue
            indent, name, tail = m.group(1), m.group(2), m.group(3)
            # Scope check: was Name declared in this file within the last 60 lines?
            decl_lines = decl_by_name.get(name, [])
            in_scope = any(d_line <= i and (i - d_line) <= 60 for d_line in decl_lines)
            if not in_scope:
                continue
            lines[i] = build(indent, name, tail)
            rewrites += 1
            break

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
