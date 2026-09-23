#!/usr/bin/env python3
"""Add functions that only a table of function pointers proves exist.

Ghidra finds functions by following calls. A function whose address is only
ever taken -- never the target of a `jal` -- is invisible to that, so it
never reaches the exported map, and `ps2_recomp` never emits it. At runtime
the guest loads the pointer, does `jalr`, and the scheduler reports a
missing target.

Dragon Quest 8's C++ static constructors are exactly this shape. crt0 walks
a 156-entry table at 0x003CF910 and `jalr`s each entry; the entries point at
0x003C8700-0x003CF8A0, a stretch that no exported function covers. It is
real MIPS code -- the first entry is the `lui/addiu/j` thunk pattern -- but
it sits inside the address range this project's notes had written off as
"junk rodata" because parts of it really are string and float data. Both
are true: the range is mixed, and the code half was being discarded with
the data half.

Carving: start at the pointer, scan forward for `jr $ra` (0x03E00008), end
one delay slot past it. Stop early at the next table entry or the next
already-mapped function, whichever comes first, and skip any pointer an
existing function already covers.

Finding a table: the addresses come from the crash itself. A
[guest-branch:missing-target] line with kind=IndirectCall reports `a0` and
`a1`, which for a ctor walk are the table's bounds. --detect scans instead,
looking for a run of word-aligned pointers into executable memory.

Usage:
    seed_pointer_table_functions.py --elf SLUS_212.07 --csv functions.csv \
        --table 0x003CF910:0x003CFB80 --prefix ctor
    seed_pointer_table_functions.py --elf SLUS_212.07 --csv functions.csv --detect
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from bisect import bisect_right
from pathlib import Path

PT_LOAD = 1
PF_X = 0x1
JR_RA = 0x03E00008
MAX_FUNCTION_BYTES = 0x4000


def load_elf(path: Path):
    blob = path.read_bytes()
    if blob[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF")
    phoff = struct.unpack_from("<I", blob, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", blob, 0x2A)
    segments = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _pa, p_filesz, _msz, p_flags, _al = struct.unpack_from(
            "<8I", blob, phoff + i * phentsize
        )
        if p_type == PT_LOAD and p_filesz:
            segments.append((p_vaddr, p_offset, p_filesz, p_flags))
    return blob, segments


def make_reader(blob, segments):
    def word(addr: int) -> int | None:
        for vaddr, offset, filesz, _flags in segments:
            if vaddr <= addr < vaddr + filesz - 3:
                return struct.unpack_from("<I", blob, offset + (addr - vaddr))[0]
        return None

    def executable(addr: int) -> bool:
        return any(
            (flags & PF_X) and vaddr <= addr < vaddr + filesz
            for vaddr, _off, filesz, flags in segments
        )

    return word, executable


def read_map(path: Path):
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
        fields = reader.fieldnames or []
    for row in rows:
        row["_start"] = int(row["Start"], 0)
        row["_end"] = int(row["End"], 0)
    rows.sort(key=lambda r: r["_start"])
    return rows, fields


def covered_by(rows, starts, addr: int) -> dict | None:
    index = bisect_right(starts, addr) - 1
    if index < 0:
        return None
    row = rows[index]
    return row if row["_start"] <= addr < row["_end"] else None


def detect_tables(word, executable, rows, minimum_entries: int):
    """Report runs of word-aligned pointers into executable memory."""
    starts = [row["_start"] for row in rows]
    highest = max(row["_end"] for row in rows)
    runs, run_start, addr = [], None, rows[0]["_start"]
    while addr < highest:
        value = word(addr)
        plausible = (
            value is not None
            and value % 4 == 0
            and executable(value)
            and covered_by(rows, starts, value) is None
        )
        if plausible and run_start is None:
            run_start = addr
        elif not plausible and run_start is not None:
            if (addr - run_start) // 4 >= minimum_entries:
                runs.append((run_start, addr))
            run_start = None
        addr += 4
    if run_start is not None and (addr - run_start) // 4 >= minimum_entries:
        runs.append((run_start, addr))
    return runs


def carve(word, start: int, hard_limit: int) -> int | None:
    """Return the end address of the function at `start`, or None if unbounded.

    Two terminators, both followed by their delay slot:

    - `jr $ra`, the ordinary return.
    - a `j` whose target lies outside [start, hard_limit) -- a tail call.
      Several ctor entries are two-instruction thunks that set up `$a0` and
      jump straight into a shared initializer, so they never contain a
      return at all. The out-of-range test is what separates those from a
      `j` used as a loop back-edge or a local jump, which must not end the
      function.
    """
    addr = start
    while addr < hard_limit and addr - start < MAX_FUNCTION_BYTES:
        raw = word(addr)
        if raw is None:
            return None
        if raw == JR_RA:
            return addr + 8
        if (raw >> 26) == 0x02:  # J
            target = (addr & 0xF0000000) | ((raw & 0x03FFFFFF) << 2)
            if target < start or target >= hard_limit:
                return addr + 8
        addr += 4
    return None


def fill_gaps(word, rows, low: int, high: int):
    """Carve functions out of every unmapped stretch between mapped functions.

    Seeding one pointer table at a time fixes one crash at a time, and the
    supply of tables is large -- vtables, ctor lists, jump tables, callback
    arrays. The underlying problem is simpler than any of them: the map has
    holes, and a hole that contains code is a hole the recompiler cannot
    emit, so any address-taken call landing in one becomes a missing target.

    Filling every hole solves the class rather than an instance. Within a
    gap: skip padding (runs of zero words), then carve to the first
    terminator, then continue from there until the gap is consumed. A stretch
    with no terminator before the end of the gap is left alone and reported
    -- that shape is data, not a truncated function.
    """
    # A handful of exported rows are degenerate (End <= Start), so treat every
    # row as occupying at least its own first word. Otherwise a gap would be
    # computed straight through a function that does exist, and the carve would
    # produce a row overlapping its Start.
    intervals = sorted((row["_start"], max(row["_end"], row["_start"] + 4)) for row in rows)
    gaps, cursor = [], low
    for start, end in intervals:
        if start > cursor:
            gaps.append((cursor, start))
        cursor = max(cursor, end)
    if cursor < high:
        gaps.append((cursor, high))

    carved, rejected = [], []
    for gap_start, gap_end in gaps:
        addr = gap_start
        while addr < gap_end:
            raw = word(addr)
            if raw is None:
                break
            if raw == 0:  # alignment padding between functions
                addr += 4
                continue
            end = carve(word, addr, gap_end)
            if end is None or end > gap_end:
                rejected.append((addr, gap_end))
                break
            carved.append((addr, end))
            addr = end
    return carved, rejected


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--table", action="append", default=[],
                        help="pointer table as START:END (repeatable)")
    parser.add_argument("--fill-gaps", metavar="LOW:HIGH",
                        help="carve functions out of every unmapped stretch in "
                             "this address range, e.g. 0x00100000:0x00387000")
    parser.add_argument("--prefix", default="ptr",
                        help="name prefix for carved functions (default: ptr)")
    parser.add_argument("--detect", action="store_true",
                        help="scan for candidate pointer tables and exit")
    parser.add_argument("--min-entries", type=int, default=16,
                        help="shortest run --detect will report (default 16)")
    parser.add_argument("--in-place", action="store_true",
                        help="write the new rows into the CSV")
    args = parser.parse_args(argv)

    blob, segments = load_elf(args.elf)
    word, executable = make_reader(blob, segments)
    rows, fields = read_map(args.csv)
    starts = [row["_start"] for row in rows]

    if args.detect:
        for begin, end in detect_tables(word, executable, rows, args.min_entries):
            print(f"0x{begin:08X}:0x{end:08X}  {(end - begin) // 4} entries")
        return 0

    if args.fill_gaps:
        try:
            low_text, high_text = args.fill_gaps.split(":")
            low, high = int(low_text, 0), int(high_text, 0)
        except ValueError:
            parser.error(f"bad --fill-gaps {args.fill_gaps!r}; want LOW:HIGH")

        carved, rejected = fill_gaps(word, rows, low, high)
        added = [
            {
                "Name": f"gap_{start:08x}",
                "Start": f"0x{start:08X}",
                "End": f"0x{end:08X}",
                "Size": str(end - start),
                "_start": start,
                "_end": end,
            }
            for start, end in carved
        ]
        for row in added[:20]:
            print(f"{row['Name']:24} {row['Start']} - {row['End']}  ({row['Size']} bytes)")
        if len(added) > 20:
            print(f"... and {len(added) - 20} more")
        for start, gap_end in rejected[:10]:
            print(f"note: 0x{start:08X}: no terminator before 0x{gap_end:08X}; left as data",
                  file=sys.stderr)
        print(f"\n{len(added)} function(s) carved from gaps, "
              f"{len(rejected)} stretch(es) left as data")

        if args.in_place and added:
            merged = sorted(rows + added, key=lambda r: r["_start"])
            for row in merged:
                row.pop("_start", None)
                row.pop("_end", None)
            with args.csv.open("w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=fields)
                writer.writeheader()
                writer.writerows(merged)
            print(f"rewrote {args.csv}")
        return 0

    if not args.table:
        parser.error("need --table START:END, --fill-gaps LOW:HIGH, or --detect")

    targets: list[int] = []
    for spec in args.table:
        try:
            begin_text, end_text = spec.split(":")
            begin, end = int(begin_text, 0), int(end_text, 0)
        except ValueError:
            parser.error(f"bad --table {spec!r}; want START:END")
        if end <= begin:
            parser.error(f"--table {spec}: end must be above start")
        for addr in range(begin, end, 4):
            value = word(addr)
            if value is None:
                print(f"warning: table entry at 0x{addr:08X} is not mapped", file=sys.stderr)
                continue
            targets.append(value)

    boundaries = sorted(set(targets) | set(starts))
    added, skipped, unbounded = [], 0, []

    for target in sorted(set(targets)):
        if not executable(target):
            print(f"warning: 0x{target:08X} is not in an executable segment; skipping",
                  file=sys.stderr)
            continue
        if covered_by(rows, starts, target) is not None:
            skipped += 1
            continue

        index = bisect_right(boundaries, target)
        hard_limit = boundaries[index] if index < len(boundaries) else target + MAX_FUNCTION_BYTES
        end = carve(word, target, hard_limit)
        if end is None or end > hard_limit:
            unbounded.append(target)
            continue

        added.append({
            "Name": f"{args.prefix}_{target:08x}",
            "Start": f"0x{target:08X}",
            "End": f"0x{end:08X}",
            "Size": str(end - target),
            "_start": target,
            "_end": end,
        })

    for row in added:
        print(f"{row['Name']:24} {row['Start']} - {row['End']}  ({row['Size']} bytes)")
    for target in unbounded:
        print(f"warning: 0x{target:08X}: no `jr $ra` before the next boundary; skipped",
              file=sys.stderr)

    print(f"\n{len(added)} function(s) added, {skipped} already mapped, "
          f"{len(unbounded)} unbounded")

    if args.in_place and added:
        merged = sorted(rows + added, key=lambda r: r["_start"])
        for row in merged:
            row.pop("_start", None)
            row.pop("_end", None)
        with args.csv.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerows(merged)
        print(f"rewrote {args.csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
