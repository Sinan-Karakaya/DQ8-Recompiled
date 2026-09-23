#!/usr/bin/env python3
"""Rename Ghidra placeholder functions using ps2_analyzer's signature matches.

ps2_analyzer identifies Sony SDK and Metrowerks MSL runtime functions by byte
signature and reports them in two lists: `stubs` (the runtime implements them)
and `untracked_stubs` (identified but recompiled normally). Both carry real
names, so a function map full of FUN_0010a2c8 placeholders can become
sceDmaSend_0010a2c8 instead. That costs nothing at build time and makes every
later boot log, stack trace and diff readable.

Only rows whose start address matches exactly are touched, and only if their
current name still looks auto-generated. Stub selection is by address, so
renaming does not disturb it.

With --add-missing it also inserts functions the analyzer found that the map
has no row for at all. Those are currently not emitted, so a call reaching one
would fail to resolve at runtime. Their end is taken as the next known function
start, which over-runs by at most the inter-function padding.

With --names it also applies a hand-curated CSV of identifications the
analyzer's signature database does not contain. Those override analyzer names
and survive the "looks auto-generated" check, because a human reading the
disassembly is better evidence than either. Keeping them in their own file is
what lets functions.csv stay free of judgement calls.

Usage:
  apply_analyzer_names.py --analyzer out.toml --csv functions.csv \
      [--names names.manual.csv] [--add-missing] [--in-place]
"""
import argparse
import bisect
import re
import sys
from collections import Counter

AUTO = re.compile(r"^(FUN|LAB|DAT|sub)_[0-9A-Fa-f]+$")


def load_names(path):
    """name -> address pairs from both analyzer stub lists."""
    out = {}
    section = None
    for line in open(path):
        s = line.strip()
        if s.startswith("stubs = [") or s.startswith("untracked_stubs = ["):
            section = s.split()[0]
            continue
        if section and s.startswith("]"):
            section = None
            continue
        if not section:
            continue
        m = re.search(r'"([^"@]+)@(0[xX][0-9A-Fa-f]+)"', s)
        if m:
            out.setdefault(int(m.group(2), 16), m.group(1))
    return out


def load_manual_names(path):
    """address -> name from a hand-curated CSV (Name,Start[,Why])."""
    import csv as _csv
    out = {}
    with open(path, newline="") as handle:
        # Leading '#' lines are commentary, not data.
        rows = [line for line in handle if not line.lstrip().startswith("#")]
    for row in _csv.DictReader(rows):
        if not row.get("Name") or not row.get("Start"):
            continue
        out[int(row["Start"], 0)] = row["Name"].strip()
    return out


def sanitize(name):
    """Make a C++-safe identifier out of an SDK or mangled MSL name."""
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--analyzer", required=True)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--names", help="hand-curated Name,Start CSV applied last")
    ap.add_argument("--in-place", action="store_true")
    ap.add_argument("--add-missing", action="store_true")
    args = ap.parse_args()

    names = load_names(args.analyzer)
    manual = load_manual_names(args.names) if args.names else {}
    lines = open(args.csv).read().splitlines()
    header, rows = lines[0], lines[1:]

    renamed = skipped_named = unmatched = manual_applied = 0
    seen = Counter()
    out = [header]
    for row in rows:
        if not row.strip():
            continue
        parts = row.split(",")
        if len(parts) < 4:
            out.append(row)
            continue
        name, start = parts[0], int(parts[1], 0)
        if start in manual:
            ident = sanitize(manual[start])
            seen[ident] += 1
            parts[0] = f"{ident}_{start:08x}"
            manual_applied += 1
            out.append(",".join(parts))
            continue
        new = names.get(start)
        if new is None:
            out.append(row)
            continue
        if not AUTO.match(name):
            # A name we derived ourselves (assert/log strings) is better
            # evidence than a signature match; keep it.
            skipped_named += 1
            out.append(row)
            continue
        ident = sanitize(new)
        # The same SDK name can legitimately match several addresses (inlined
        # or duplicated helpers), so keep identifiers unique.
        seen[ident] += 1
        parts[0] = f"{ident}_{start:08x}"
        renamed += 1
        out.append(",".join(parts))

    parsed = [r.split(",") for r in rows if len(r.split(",")) >= 4]
    spans = sorted((int(p[1], 0), int(p[2], 0)) for p in parsed)
    starts = [s for s, _ in spans]
    matched_addrs = set(starts)

    def covered(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and spans[i][0] < addr < spans[i][1]

    absent = sorted(a for a in names if a not in matched_addrs and not covered(a))
    added = 0
    if args.add_missing and absent:
        # End at whichever comes first: the next analyzer function or the next
        # row already in the map.
        boundaries = sorted(set(starts) | set(absent))
        for addr in absent:
            j = bisect.bisect_right(boundaries, addr)
            if j >= len(boundaries):
                continue
            end = boundaries[j]
            ident = sanitize(names[addr])
            out.append(f"{ident}_{addr:08x},0x{addr:08X},0x{end:08X},{end - addr}")
            added += 1
        body = out[1:]
        body.sort(key=lambda r: int(r.split(",")[1], 0))
        out = [out[0]] + body

    sys.stderr.write(
        f"manual names={len(manual)} applied={manual_applied} "
        f"analyzer names={len(names)} renamed={renamed} "
        f"kept-existing={skipped_named} absent-from-map={len(absent)} "
        f"added={added}\n"
    )
    dupes = [n for n, c in seen.items() if c > 1]
    if dupes:
        sys.stderr.write(f"names matching multiple addresses: {len(dupes)}"
                         f" (e.g. {', '.join(sorted(dupes)[:5])})\n")

    text = "\n".join(out) + "\n"
    if args.in_place:
        open(args.csv, "w").write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
