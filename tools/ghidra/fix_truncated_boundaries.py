#!/usr/bin/env python3
"""Find and repair functions whose end address was cut short in the map.

CodeWarrior routinely moves a cold basic block -- a NULL check's early-out,
an error path -- past the function's main `jr $ra`, sometimes with a few
words of alignment padding in between. Ghidra's analyser tends to end the
function at that first `jr $ra`, leaving the out-of-line tail outside every
function in the exported map.

That is not a cosmetic problem. `ps2_recomp` decides whether a branch is
internal by testing the target against the owning function's bounds, so a
relative branch into a truncated tail is classified as *external* and
compiled to

    ctx->pc = <target>;
    return;

which hands control to `EeScheduler`, which looks for a function registered
at exactly that address, finds none, and makes the thread dormant. The
symptom is the guest stopping dead at a mid-function address with no
diagnostic beyond `[guest-branch:missing-target]`.

Detection is exact rather than heuristic, because PC-relative branches on
MIPS cannot cross a function boundary in compiler output: if a function
contains a relative branch to an address that lies *after* its recorded end
but *before* the next function starts, the recorded end is wrong. Jumps
(`j`/`jal`) are deliberately ignored -- those legitimately leave a function.

Repair extends the end to cover the target's delay slot, then re-scans,
because a newly included tail can itself branch further on. Extension never
crosses into the next mapped function; if it would, the case is reported and
left alone for a human.

A second, unrelated defect gets the same treatment: an end that lands on a
branch's delay slot instead of past it. `ps2_recomp` only decodes [start,
end), so the delay slot is dropped and the instruction never runs -- for a
`jr $ra` epilogue that means the `addiu $sp, $sp, N` never executes and every
call leaks stack.

Usage:
    fix_truncated_boundaries.py --elf SLUS_212.07 --csv functions.csv
    fix_truncated_boundaries.py --elf SLUS_212.07 --csv functions.csv --in-place
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

PT_LOAD = 1

# I-type opcodes whose 16-bit immediate is a PC-relative branch offset.
BRANCH_OPCODES = {
    0x04, 0x05, 0x06, 0x07,  # BEQ  BNE  BLEZ  BGTZ
    0x14, 0x15, 0x16, 0x17,  # BEQL BNEL BLEZL BGTZL
}
# COP1/COP2 branch-on-condition: opcode 0x11/0x12 with rs == 0x08 (BC1/BC2).
COP_BRANCH_OPCODES = {0x11, 0x12}
REGIMM_OPCODE = 0x01
# REGIMM rt values that are branches (BLTZ/BGEZ/BLTZL/BGEZL and their AL forms).
REGIMM_BRANCH_RT = {0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13}


@dataclass
class Segment:
    vaddr: int
    offset: int
    filesz: int


def load_segments(path: Path) -> list[Segment]:
    blob = path.read_bytes()
    if blob[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF")
    phoff = struct.unpack_from("<I", blob, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", blob, 0x2A)
    segments = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _pa, p_filesz, _msz, _fl, _al = struct.unpack_from(
            "<8I", blob, phoff + i * phentsize
        )
        if p_type == PT_LOAD and p_filesz:
            segments.append(Segment(p_vaddr, p_offset, p_filesz))
    return segments, blob


def word_at(blob: bytes, segments: list[Segment], addr: int) -> int | None:
    for seg in segments:
        if seg.vaddr <= addr < seg.vaddr + seg.filesz - 3:
            off = seg.offset + (addr - seg.vaddr)
            return struct.unpack_from("<I", blob, off)[0]
    return None


def branch_target(addr: int, raw: int) -> int | None:
    """Return the PC-relative branch target of `raw`, or None if not a branch."""
    opcode = raw >> 26
    if opcode in BRANCH_OPCODES:
        pass
    elif opcode == REGIMM_OPCODE and ((raw >> 16) & 0x1F) in REGIMM_BRANCH_RT:
        pass
    elif opcode in COP_BRANCH_OPCODES and ((raw >> 21) & 0x1F) == 0x08:
        pass
    else:
        return None

    offset = raw & 0xFFFF
    if offset & 0x8000:
        offset -= 0x10000
    return addr + 4 + offset * 4


def has_delay_slot(raw: int) -> bool:
    """True if `raw` is a branch or jump, i.e. the next word is its delay slot."""
    opcode = raw >> 26
    if opcode == 0x00:
        return (raw & 0x3F) in (0x08, 0x09)  # JR JALR
    if opcode in (0x02, 0x03):  # J JAL
        return True
    return branch_target(0, raw) is not None


MAX_TAIL_BYTES = 0x1000


def repair_outlined_tails(rows: list[dict], blob: bytes, segments: list[Segment]):
    """Attach unmapped out-of-line tails to the function they branch back into.

    CodeWarrior parks a function's cold blocks after *other* functions, so the
    tail is neither inside its owner nor adjacent to it and `analyse` cannot
    reach it without crossing the function in between. The tail identifies its
    owner unambiguously: a PC-relative branch cannot cross a function boundary
    in compiler output, so a branch from the hole into F means the hole is F's.

    Extension therefore overlaps whatever sits in between. That is harmless --
    `ps2_recomp` decodes each function independently and the duplicated body is
    unreachable inside the extended one -- and it is the only way to make the
    tail's branches back into F resolve as internal labels instead of dispatches
    to an address no function is registered at.
    """
    covered = sorted((row["_start"], row["_end"]) for row in rows if row["_end"] > row["_start"])
    holes = []
    cursor = covered[0][1] if covered else 0
    for start, end in covered[1:]:
        if start > cursor:
            holes.append((cursor, start))
        cursor = max(cursor, end)

    repairs = []
    for hole_start, hole_end in holes:
        if hole_end - hole_start > MAX_TAIL_BYTES:
            continue
        owners = set()
        for addr in range(hole_start, hole_end, 4):
            raw = word_at(blob, segments, addr)
            if raw is None:
                owners.clear()
                break
            target = branch_target(addr, raw)
            if target is None or hole_start <= target < hole_end:
                continue
            for index, row in enumerate(rows):
                if row["_start"] < target < row["_end"] and row["_end"] <= hole_start:
                    owners.add(index)
        for index in owners:
            row = rows[index]
            repairs.append((row, row["_end"], hole_end))
            row["_end"] = hole_end
            row["End"] = f"0x{hole_end:08X}"
            row["Size"] = str(hole_end - row["_start"])
    return repairs


def read_map(path: Path) -> tuple[list[dict], list[str]]:
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
        fields = reader.fieldnames or []
    for row in rows:
        row["_start"] = int(row["Start"], 0)
        row["_end"] = int(row["End"], 0)
    rows.sort(key=lambda r: r["_start"])
    return rows, fields


JR_RA = 0x03E00008


def repair_degenerate(rows: list[dict], blob: bytes, segments: list[Segment]):
    """Give an end address to rows the export left with End <= Start.

    A few exported rows carry an end that precedes their start, sometimes by
    tens of kilobytes -- an artefact of the exporter, not of the binary.
    `ps2_recomp` sees an empty range and emits nothing, so those addresses
    behave exactly like unmapped ones. Re-derive the end by scanning to the
    first `jr $ra`, bounded by the next function.
    """
    starts = sorted(row["_start"] for row in rows)
    fixed = []
    for row in rows:
        if row["_end"] > row["_start"]:
            continue
        limit = next((s for s in starts if s > row["_start"]), row["_start"] + 0x4000)
        addr = row["_start"]
        end = None
        while addr < limit:
            raw = word_at(blob, segments, addr)
            if raw is None:
                break
            if raw == JR_RA:
                end = min(addr + 8, limit)
                break
            addr += 4
        if end is None:
            end = min(row["_start"] + 8, limit)
        fixed.append((row, row["_end"], end))
        row["_end"] = end
        row["End"] = f"0x{end:08X}"
        row["Size"] = str(end - row["_start"])
    return fixed


def repair_delay_slots(rows: list[dict], blob: bytes, segments: list[Segment]):
    """Extend ends that stop on a branch, so its delay slot stays inside."""
    starts = sorted(row["_start"] for row in rows)
    fixed, blocked = [], []
    for row in rows:
        end = row["_end"]
        if end <= row["_start"]:
            continue
        raw = word_at(blob, segments, end - 4)
        if raw is None or not has_delay_slot(raw):
            continue
        limit = next((s for s in starts if s >= end), None)
        if limit is not None and end + 4 > limit:
            blocked.append((row, end, limit))
            continue
        fixed.append((row, end, end + 4))
        row["_end"] = end + 4
        row["End"] = f"0x{end + 4:08X}"
        row["Size"] = str(end + 4 - row["_start"])
    return fixed, blocked


def analyse(rows: list[dict], blob: bytes, segments: list[Segment]):
    """Return (repairs, blocked) after iterating each function to a fixpoint."""
    starts = [row["_start"] for row in rows]
    repairs, blocked = [], []

    for index, row in enumerate(rows):
        # The first mapped function starting after this one bounds any growth.
        limit = None
        for candidate in starts[index + 1 :]:
            if candidate >= row["_end"]:
                limit = candidate
                break

        original_end = row["_end"]
        end = original_end
        while True:
            furthest = end
            addr = row["_start"]
            while addr < end:
                raw = word_at(blob, segments, addr)
                if raw is None:
                    break
                target = branch_target(addr, raw)
                # +8 covers the target instruction and its delay slot.
                if target is not None and end <= target and target + 8 > furthest:
                    furthest = target + 8
                addr += 4
            if furthest == end:
                break
            if limit is not None and furthest > limit:
                blocked.append((row, original_end, furthest, limit))
                break
            end = furthest

        if end != original_end:
            row["_end"] = end
            row["End"] = f"0x{end:08X}"
            row["Size"] = str(end - row["_start"])
            repairs.append((row, original_end, end))

    return repairs, blocked


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--in-place", action="store_true",
                        help="rewrite the CSV instead of only reporting")
    parser.add_argument("--limit", type=int, default=20,
                        help="how many repairs to print (default 20; 0 for all)")
    args = parser.parse_args(argv)

    segments, blob = load_segments(args.elf)
    rows, fields = read_map(args.csv)

    degenerate = repair_degenerate(rows, blob, segments)
    for row, old_end, new_end in degenerate:
        print(f"{row['Name']:40} 0x{row['_start']:08x}  "
              f"degenerate end 0x{old_end:08x} -> 0x{new_end:08x}")

    tails = repair_outlined_tails(rows, blob, segments)
    for row, old_end, new_end in (tails if args.limit == 0 else tails[: args.limit]):
        print(f"{row['Name']:40} 0x{row['_start']:08x}  "
              f"outlined tail 0x{old_end:08x} -> 0x{new_end:08x}")
    if args.limit and len(tails) > args.limit:
        print(f"... and {len(tails) - args.limit} more outlined-tail repairs")

    repairs, blocked = analyse(rows, blob, segments)

    # Last, so it also covers ends that analyse() just moved onto a delay slot.
    slots, slot_blocked = repair_delay_slots(rows, blob, segments)
    for row, old_end, new_end in (slots if args.limit == 0 else slots[: args.limit]):
        print(f"{row['Name']:40} 0x{row['_start']:08x}  "
              f"delay slot 0x{old_end:08x} -> 0x{new_end:08x}")
    if args.limit and len(slots) > args.limit:
        print(f"... and {len(slots) - args.limit} more delay-slot repairs")
    for row, end, limit in slot_blocked:
        print(
            f"BLOCKED {row['Name']} 0x{row['_start']:08x}: ends on a delay slot at "
            f"0x{end:08x} but 0x{limit:08x} is the next mapped function",
            file=sys.stderr,
        )

    shown = repairs if args.limit == 0 else repairs[: args.limit]
    for row, old_end, new_end in shown:
        print(
            f"{row['Name']:40} 0x{row['_start']:08x}  "
            f"end 0x{old_end:08x} -> 0x{new_end:08x}  (+{new_end - old_end} bytes)"
        )
    if len(repairs) > len(shown):
        print(f"... and {len(repairs) - len(shown)} more")

    for row, old_end, wanted, limit in blocked:
        print(
            f"BLOCKED {row['Name']} 0x{row['_start']:08x}: wants end 0x{wanted:08x} "
            f"but 0x{limit:08x} is the next mapped function (end stays 0x{old_end:08x})",
            file=sys.stderr,
        )

    print(f"\n{len(repairs)} function(s) extended, {len(degenerate)} degenerate "
          f"end(s) rebuilt, {len(tails)} outlined tail(s) attached, "
          f"{len(slots)} delay slot(s) recovered, "
          f"{len(blocked) + len(slot_blocked)} blocked, {len(rows)} total")

    if args.in_place and (repairs or degenerate or slots or tails):
        for row in rows:
            for key in ("_start", "_end"):
                row.pop(key, None)
        with args.csv.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
        print(f"rewrote {args.csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
