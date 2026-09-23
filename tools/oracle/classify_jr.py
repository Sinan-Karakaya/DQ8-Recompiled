#!/usr/bin/env python3
"""Classify each unresolved JR/JALR site reported by ps2_recomp.

JALR is an indirect call: a jump table cannot resolve it, and a single resume
entry at the return address is the correct handling. JR (with rs != ra) is the
switch-dispatch case that jump-table data could resolve.
"""
import argparse
import re
import struct
from collections import Counter

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", help="ps2_recomp run log")
parser.add_argument("--elf", required=True, help="user-supplied SLUS_212.07 ELF")
args = parser.parse_args()
# This diagnostic uses the NTSC-U resident segment layout.
SEG_VADDR, SEG_OFF, SEG_SIZE = 0x100000, 0x180, 0x2D2680

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

with open(args.elf, "rb") as handle:
    blob = handle.read()


def word_at(vaddr):
    if not (SEG_VADDR <= vaddr < SEG_VADDR + SEG_SIZE):
        return None
    off = SEG_OFF + (vaddr - SEG_VADDR)
    return struct.unpack_from("<I", blob, off)[0]


kinds = Counter()
rs_hist = Counter()
jr_sites, jalr_sites = [], []
fallback_total = 0
funcs = set()

pat = re.compile(r"function=(\S+) addr=0x([0-9a-f]+) - unresolved JR/JALR at ([0-9a-fx ]+?); promoted (\d+)")
for line in open(args.log, encoding="utf-8", errors="replace"):
    m = pat.search(line)
    if not m:
        continue
    fname, _, addrs, promoted = m.groups()
    funcs.add(fname)
    fallback_total += int(promoted)
    for a in addrs.split():
        va = int(a, 16)
        w = word_at(va)
        if w is None:
            kinds["<outside segment>"] += 1
            continue
        op, func = w >> 26, w & 0x3F
        rs = (w >> 21) & 0x1F
        if op != 0:
            kinds[f"<not SPECIAL op=0x{op:02x}>"] += 1
            continue
        if func == 0x08:
            kinds["JR"] += 1
            rs_hist["JR $" + REG[rs]] += 1
            jr_sites.append((va, REG[rs], fname))
        elif func == 0x09:
            kinds["JALR"] += 1
            rs_hist["JALR $" + REG[rs]] += 1
            jalr_sites.append((va, REG[rs], fname))
        else:
            kinds[f"<SPECIAL func=0x{func:02x}>"] += 1

print(f"warned functions           : {len(funcs)}")
print(f"promoted fallback entries  : {fallback_total}")
print(f"unresolved sites total     : {sum(kinds.values())}")
print()
print("=== site kind ===")
for k, v in kinds.most_common():
    print(f"  {k:28s} {v}")
print()
print("=== source register ===")
for k, v in rs_hist.most_common(12):
    print(f"  {k:28s} {v}")
print()
print(f"JR sites (jump-table candidates): {len(jr_sites)}")
for va, rs, fn in jr_sites[:15]:
    print(f"  0x{va:08x}  jr ${rs:3s}  in {fn}")
