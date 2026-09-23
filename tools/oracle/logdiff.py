#!/usr/bin/env python3
"""logdiff.py -- compare a DQ8Recomp runtime log against the golden trace.

METHODOLOGY
-----------
The "golden trace" is a console-log capture of the real, unmodified NTSC-U
retail game (SLUS_212.07) running in PCSX2 with its two stubbed debug-log
call sites re-enabled (see tools/oracle/pnach/SLUS-21207_F4715852.pnach and
https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing). It logs, in the exact order the real hardware/BIOS/game
code produces them: file loads (name, offset, size), VRAM/texture
allocations, IOP memory allocations, and thread/semaphore create/delete --
i.e. the real boot sequence, asset load order, and thread model of Dragon
Quest VIII.

Our recompiled runtime is expected to be *sparse* for a long time: early on
it may only implement a handful of the syscalls/SDK calls that produce
these log lines. A naive full-file diff would therefore report a "different
from line 1" divergence forever and be useless. Instead this tool:

  1. Optionally restricts comparison to one or more *categories* of log
     line via --subset (file loads, VRAM/texture, thread/semaphore, IOP
     allocations, audio/movie, or "other"), filtering *both* logs down to
     just that category before aligning them. This lets you validate "do
     we load files in the same order" independently of "do we create
     threads in the same order", well before every category is implemented.
  2. Normalizes volatile fields before comparing -- the PCSX2 log's leading
     "[ 12.3456]" host wall-clock timestamp, and raw hex memory
     addresses/pointers embedded in a handful of message formats -- which
     legitimately differ between two runs/implementations and are not
     meaningful behavioral divergences.
  3. Reports the FIRST divergence only, with surrounding context, since
     that is almost always the one worth chasing; everything after a real
     divergence is usually noise (the two executions have simply diverged).

USAGE
-----
    logdiff.py --golden GOLDEN --candidate CANDIDATE [--subset CATS]
               [--context N] [--strict-ids]
    logdiff.py --list-categories
    logdiff.py --self-test [--golden GOLDEN]

Exit status: 0 if the compared streams matched completely (or all
self-tests passed), 1 if a divergence was found (or any self-test failed),
2 on usage/IO errors.

EXAMPLES
--------
    # Full comparison once the runtime log exists:
    tools/oracle/logdiff.py --golden Assets/oracle/boot_us.log \\
        --candidate build/runtime_boot.log

    # Only check file-load order/sizes -- useful long before threads or
    # VRAM allocation are implemented:
    tools/oracle/logdiff.py --golden Assets/oracle/boot_us.log \\
        --candidate build/runtime_boot.log --subset file

    # Prove the tool itself works, with no runtime log required:
    tools/oracle/logdiff.py --self-test
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# Categories
#
# Each category is a regex matched against the line *after* the timestamp
# prefix has been stripped, but *before* volatile-field normalization (the
# category-defining tokens themselves are never volatile). Order matters:
# first match wins, and "other" (checked last) catches everything else.
# --------------------------------------------------------------------------
CATEGORIES: list[tuple[str, re.Pattern[str]]] = [
    ("file", re.compile(r"^(load\(\d+\)\s|file cache\b)")),
    ("vram", re.compile(r"^(Top Vram\b|vram \d|tex \d+/\d+|b = )")),
    ("thread", re.compile(r"^(create sema\b|delete sema\b|start thread\b|delete thread\b)")),
    ("iop", re.compile(r"^IOP memory\b")),
    ("audio", re.compile(
        r"^(fstrm\s*:|sndstrm\s*:|PlayBGM\b|mc finish|def_vol|loop \d|movie vol)")),
]
CATEGORY_NAMES = [name for name, _ in CATEGORIES] + ["other"]


def categorize(line: str) -> str:
    for name, pattern in CATEGORIES:
        if pattern.search(line):
            return name
    return "other"


# --------------------------------------------------------------------------
# Normalization -- strips fields that legitimately differ run-to-run.
# --------------------------------------------------------------------------

# PCSX2's own per-line host wall-clock timestamp, e.g. "[   43.2897] ".
# Not part of the game's output at all; never meaningful to compare.
_TIMESTAMP_RE = re.compile(r"^\[\s*[\d.]+\]\s?")

# Raw hex pointers/addresses, both "0x"-prefixed (IOP alloc addresses like
# "0x000cee00") and bare name=hex fields the game emits for heap pointers
# (e.g. "hd=cc600,bd=1bc980,hdbd=0", "mwork = 1325980" is decimal so is left
# alone deliberately -- only the hex forms are ambiguous enough with real
# counters to normalize).
_HEX_ADDR_RE = re.compile(r"0x[0-9A-Fa-f]{4,}")
_NAMED_HEX_RE = re.compile(r"\b(hd|bd|hdbd)=[0-9A-Fa-f]+")

NORMALIZERS: list[tuple[re.Pattern[str], str]] = [
    (_HEX_ADDR_RE, "<HEX>"),
    (_NAMED_HEX_RE, r"\1=<HEX>"),
]


def strip_timestamp(line: str) -> str:
    return _TIMESTAMP_RE.sub("", line, count=1)


def normalize(line: str, *, strict_ids: bool = False) -> str:
    """Canonicalize a line for comparison: drop the PCSX2 timestamp, fold
    volatile hex addresses to a placeholder. Thread/semaphore/texture slot
    *numbers* are intentionally left alone (they are meaningful ordering
    behavior we want to catch divergences in) unless the caller passes
    strict_ids=False's opposite is requested some day; --strict-ids on the
    CLI is reserved for turning this normalization off entirely (exact byte
    comparison), see --help.
    """
    out = strip_timestamp(line)
    if not strict_ids:
        for pattern, repl in NORMALIZERS:
            out = pattern.sub(repl, out)
    return out


# --------------------------------------------------------------------------
# Comparison
# --------------------------------------------------------------------------

@dataclass
class Entry:
    orig_index: int  # 1-based line number in the source file
    raw: str
    category: str


def load_entries(path: str) -> list[Entry]:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    out = []
    for i, raw in enumerate(lines, start=1):
        body = strip_timestamp(raw)
        out.append(Entry(i, raw, categorize(body)))
    return out


def filter_categories(entries: list[Entry], categories: set[str] | None) -> list[Entry]:
    if categories is None:
        return entries
    return [e for e in entries if e.category in categories]


@dataclass
class DivergenceReport:
    matched: int  # number of leading entries that compared equal
    golden_total: int
    candidate_total: int
    kind: str = "match"  # "match" | "diff" | "golden_ended" | "candidate_ended"
    golden_entry: Entry | None = None
    candidate_entry: Entry | None = None
    context: list[tuple[Entry, Entry]] = field(default_factory=list)


def compare(golden: list[Entry], candidate: list[Entry], *, context_n: int,
            strict_ids: bool) -> DivergenceReport:
    n = min(len(golden), len(candidate))
    matched_pairs: list[tuple[Entry, Entry]] = []
    for i in range(n):
        g, c = golden[i], candidate[i]
        if normalize(g.raw, strict_ids=strict_ids) != normalize(c.raw, strict_ids=strict_ids):
            return DivergenceReport(
                matched=i, golden_total=len(golden), candidate_total=len(candidate),
                kind="diff", golden_entry=g, candidate_entry=c,
                context=matched_pairs[-context_n:] if context_n else [],
            )
        matched_pairs.append((g, c))

    if len(golden) == len(candidate):
        return DivergenceReport(matched=n, golden_total=len(golden), candidate_total=len(candidate))

    if len(golden) > len(candidate):
        return DivergenceReport(
            matched=n, golden_total=len(golden), candidate_total=len(candidate),
            kind="candidate_ended", golden_entry=golden[n],
            context=matched_pairs[-context_n:] if context_n else [],
        )

    return DivergenceReport(
        matched=n, golden_total=len(golden), candidate_total=len(candidate),
        kind="golden_ended", candidate_entry=candidate[n],
        context=matched_pairs[-context_n:] if context_n else [],
    )


def format_report(report: DivergenceReport, *, subset_label: str) -> str:
    lines = []
    lines.append(f"logdiff: comparing subset={subset_label}")
    lines.append(f"  golden entries in subset:    {report.golden_total}")
    lines.append(f"  candidate entries in subset: {report.candidate_total}")

    if report.kind == "match":
        lines.append(f"MATCH: all {report.matched} compared entries agree.")
        return "\n".join(lines)

    lines.append(f"{report.matched} leading entries matched.")
    if report.context:
        lines.append("")
        lines.append("-- context (last matched entries) --")
        for g, c in report.context:
            lines.append(f"  golden#{g.orig_index:<6} candidate#{c.orig_index:<6} [{g.category}] {g.raw}")

    lines.append("")
    if report.kind == "diff":
        g, c = report.golden_entry, report.candidate_entry
        lines.append(f"FIRST DIVERGENCE at compared position {report.matched} (category={g.category}):")
        lines.append(f"  golden    #{g.orig_index}: {g.raw}")
        lines.append(f"  candidate #{c.orig_index}: {c.raw}")
    elif report.kind == "candidate_ended":
        g = report.golden_entry
        lines.append(f"CANDIDATE ENDED after {report.matched} matching entries.")
        lines.append(f"  golden continues at #{g.orig_index} with [{g.category}]: {g.raw}")
        lines.append("  (this is expected/benign while the runtime is still sparse -- "
                      "use --subset to check only categories it implements)")
    elif report.kind == "golden_ended":
        c = report.candidate_entry
        lines.append(f"CANDIDATE HAS EXTRA ENTRIES after {report.matched} matching entries "
                      f"(golden ended first).")
        lines.append(f"  candidate continues at #{c.orig_index} with [{c.category}]: {c.raw}")
        lines.append("  (candidate produced output the real game never did -- likely a real bug)")
    return "\n".join(lines)


def run_diff(golden_path: str, candidate_path: str, *, subset: set[str] | None,
             context_n: int, strict_ids: bool) -> int:
    golden = filter_categories(load_entries(golden_path), subset)
    candidate = filter_categories(load_entries(candidate_path), subset)
    report = compare(golden, candidate, context_n=context_n, strict_ids=strict_ids)
    subset_label = "all" if subset is None else ",".join(sorted(subset))
    print(format_report(report, subset_label=subset_label))
    return 0 if report.kind == "match" else 1


# --------------------------------------------------------------------------
# Self-test -- proves the comparator works without needing our runtime or
# even the real captured golden trace to exist. Uses a small embedded
# fixture covering every category, unless --golden points at a real file
# (in which case that file is used as the base for perturbation instead).
# --------------------------------------------------------------------------

_FIXTURE = """\
Top Vram = 10752(64word)
load(0) bin/key_data_1.bin o = 0 s = 2048
file cache itemdat2.bin
create sema 27
create sema 26
start thread 9
IOP memory 0x000cee00(size:24576) is allocated
tex 1/288
b = 32767,cblur_fix_work0 vram = 64,0
fstrm : open cddat:WAV¥BGM¥BG_002.MWV
delete thread 9
delete sema 26
load(0) bin/bin_ext2.pak o = 0 s = 694272
"""


def _write_lines(path: str, lines: list[str]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def self_test(golden_path: str | None) -> int:
    import tempfile
    import os

    if golden_path:
        with open(golden_path, "r", encoding="utf-8", errors="replace") as f:
            base_lines = f.read().splitlines()
        source_label = golden_path
    else:
        base_lines = _FIXTURE.splitlines()
        source_label = "<built-in fixture>"

    if len(base_lines) < 4:
        print(f"self-test: {source_label} has too few lines ({len(base_lines)}) to perturb", file=sys.stderr)
        return 2

    failures = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal failures
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name}" + (f" -- {detail}" if detail and not ok else ""))
        if not ok:
            failures += 1

    with tempfile.TemporaryDirectory(prefix="logdiff_selftest_") as tmp:
        golden_copy = os.path.join(tmp, "golden.log")
        _write_lines(golden_copy, base_lines)

        # 1. Identical copy must match exactly.
        identical = os.path.join(tmp, "identical.log")
        _write_lines(identical, base_lines)
        g = load_entries(golden_copy)
        c = load_entries(identical)
        r = compare(g, c, context_n=3, strict_ids=False)
        check("identical copy matches", r.kind == "match")

        # 2. Perturb one line's content (not just a volatile field) partway
        #    through the file; the comparator must flag exactly that line.
        mutate_at = len(base_lines) // 2
        perturbed_lines = list(base_lines)
        perturbed_lines[mutate_at] = perturbed_lines[mutate_at] + " MUTATED"
        perturbed = os.path.join(tmp, "perturbed.log")
        _write_lines(perturbed, perturbed_lines)
        c2 = load_entries(perturbed)
        r2 = compare(g, c2, context_n=3, strict_ids=False)
        check(
            "mutation is caught at the correct line",
            r2.kind == "diff" and r2.matched == mutate_at,
            f"expected first divergence at compared position {mutate_at}, got kind={r2.kind} matched={r2.matched}",
        )

        # 3. Volatile hex addresses must NOT trigger a divergence.
        hex_line_idx = next((i for i, l in enumerate(base_lines) if _HEX_ADDR_RE.search(l)), None)
        if hex_line_idx is not None:
            volatile_lines = list(base_lines)
            volatile_lines[hex_line_idx] = _HEX_ADDR_RE.sub("0xDEADBEEF", volatile_lines[hex_line_idx], count=1)
            volatile_path = os.path.join(tmp, "volatile.log")
            _write_lines(volatile_path, volatile_lines)
            c3 = load_entries(volatile_path)
            r3 = compare(g, c3, context_n=3, strict_ids=False)
            check("normalized hex address does not cause a false divergence", r3.kind == "match")

            # 3b. But with --strict-ids (raw comparison), it must be caught.
            r3b = compare(g, c3, context_n=3, strict_ids=True)
            check("--strict-ids catches the same hex-address change", r3b.kind != "match")
        else:
            print("[SKIP] no hex-address line in fixture to test normalization")

        # 4. Truncated candidate must be reported as candidate_ended, not a
        #    false "diff", and at the right matched count.
        truncate_at = max(1, len(base_lines) - 3)
        truncated = os.path.join(tmp, "truncated.log")
        _write_lines(truncated, base_lines[:truncate_at])
        c4 = load_entries(truncated)
        r4 = compare(g, c4, context_n=3, strict_ids=False)
        check(
            "truncated candidate reported as candidate_ended",
            r4.kind == "candidate_ended" and r4.matched == truncate_at,
            f"got kind={r4.kind} matched={r4.matched}, expected {truncate_at}",
        )

        # 5. --subset filtering: a mutation OUTSIDE the selected category
        #    must be invisible; a mutation INSIDE it must be caught.
        target_cat = categorize(strip_timestamp(base_lines[mutate_at]))
        other_cat_idx = next(
            (i for i, l in enumerate(base_lines) if categorize(strip_timestamp(l)) != target_cat), None)
        if other_cat_idx is not None:
            g_sub = filter_categories(load_entries(golden_copy), {target_cat})
            c_sub_same = filter_categories(load_entries(perturbed), {target_cat})
            r5 = compare(g_sub, c_sub_same, context_n=3, strict_ids=False)
            check(
                f"--subset={target_cat} still catches an in-category mutation",
                r5.kind == "diff",
            )

            other_perturbed_lines = list(base_lines)
            other_perturbed_lines[other_cat_idx] += " MUTATED"
            other_cat = categorize(strip_timestamp(base_lines[other_cat_idx]))
            other_path = os.path.join(tmp, "other_perturbed.log")
            _write_lines(other_path, other_perturbed_lines)
            g_sub2 = filter_categories(load_entries(golden_copy), {target_cat})
            c_sub2 = filter_categories(load_entries(other_path), {target_cat})
            r6 = compare(g_sub2, c_sub2, context_n=3, strict_ids=False)
            check(
                f"--subset={target_cat} ignores a mutation in category={other_cat}",
                r6.kind == "match",
            )
        else:
            print("[SKIP] fixture has only one category, cannot test --subset isolation")

    print()
    print(f"self-test source: {source_label}")
    if failures:
        print(f"SELF-TEST FAILED: {failures} check(s) failed.")
        return 1
    print("SELF-TEST PASSED: all checks ok.")
    return 0


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="logdiff.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--golden", metavar="PATH",
                    help="path to the golden-trace log (default: Assets/oracle/boot_us.log "
                         "relative to the repo root's sibling Assets dir; not required for "
                         "--self-test, and optional there too -- see --self-test).")
    p.add_argument("--candidate", metavar="PATH",
                    help="path to the log produced by our runtime. Required unless --self-test "
                         "or --list-categories is given.")
    p.add_argument("--subset", metavar="CAT[,CAT...]",
                    help="only compare log lines in these categories (comma-separated). "
                         "See --list-categories for the full list. Default: compare everything.")
    p.add_argument("--context", type=int, default=5, metavar="N",
                    help="number of matched lines of context to print before a divergence "
                         "(default: 5).")
    p.add_argument("--strict-ids", action="store_true",
                    help="disable volatile-field normalization (raw byte-for-byte line "
                         "comparison after only stripping the PCSX2 timestamp prefix). "
                         "Useful to confirm a normalization rule is actually doing something.")
    p.add_argument("--list-categories", action="store_true",
                    help="print the known --subset category names and their matching rule, then exit.")
    p.add_argument("--self-test", action="store_true",
                    help="run the tool's own correctness checks (diffing a perturbed copy of a "
                         "golden trace against itself) and exit; proves logdiff works even "
                         "before our runtime exists. Uses a small built-in fixture unless "
                         "--golden is also given, in which case that real file is used as the "
                         "base for perturbation.")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    if args.list_categories:
        print("Categories (checked in this order; first match wins, 'other' catches the rest):")
        for name, pattern in CATEGORIES:
            print(f"  {name:<8} {pattern.pattern}")
        print(f"  {'other':<8} (anything not matched above)")
        return 0

    if args.self_test:
        return self_test(args.golden)

    if not args.candidate:
        print("logdiff.py: --candidate is required (unless --self-test or --list-categories)",
              file=sys.stderr)
        return 2
    golden_path = args.golden or "Assets/oracle/boot_us.log"
    subset = set(s.strip() for s in args.subset.split(",")) if args.subset else None
    if subset:
        unknown = subset - set(CATEGORY_NAMES)
        if unknown:
            print(f"logdiff.py: unknown --subset categories: {', '.join(sorted(unknown))}\n"
                  f"  known categories: {', '.join(CATEGORY_NAMES)}", file=sys.stderr)
            return 2

    try:
        return run_diff(golden_path, args.candidate, subset=subset,
                         context_n=args.context, strict_ids=args.strict_ids)
    except OSError as e:
        print(f"logdiff.py: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
