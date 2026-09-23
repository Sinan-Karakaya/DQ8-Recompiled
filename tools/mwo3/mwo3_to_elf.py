#!/usr/bin/env python3
"""Wrap a Metrowerks MWo3 overlay (BIN/*.BIN) in a minimal MIPS ELF.

Dragon Quest 8 keeps six of its subsystems out of the main executable and
loads them on demand into a fixed 160 KB arena. The files in `BIN/` are not
ELFs -- they are Metrowerks linker overlays, a 0x40-byte header followed by
raw `.text` and `.data` images, with `.bss` left implicit. Neither Ghidra
nor `ps2_recomp` can open them directly, so this script rewrites one into
the smallest valid ET_EXEC ELF that carries the same bytes at the same
addresses.

Header layout (little-endian, all fields verified against all six retail
NTSC-U overlays -- `0x40 + text_size + data_size == filesize` holds exactly
for every one of them):

    0x00  char[4]   "MWo3"
    0x04  u32       overlay id      1 casino, 2 viewer, 3 shop,
                                    4 menu,   5 battle, 6 title
    0x08  u32       load address    0x00461C80 for every NTSC-U overlay;
                                    .text therefore begins at 0x00461CC0
    0x0c  u32       .text size
    0x10  u32       .data size
    0x14  u32       .bss size       not present in the file
    0x18  u32       static ctor table start   (inside .data)
    0x1c  u32       static ctor table end
    0x20  char[32]  lowercase name, NUL-padded ("title.bin"), runs to 0x40

Every overlay loads at the same address, so their code overlaps by design
and only one can be resident at a time. That is why each needs its own
translation unit namespace and a runtime overlay manager rather than being
merged into the main function table -- see
https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Architecture.

Usage:
    mwo3_to_elf.py BIN/TITLE.BIN -o build/overlays/title.elf
    mwo3_to_elf.py --info BIN/*.BIN
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

MWO3_MAGIC = b"MWo3"
HEADER_SIZE = 0x40

EM_MIPS = 8
ET_EXEC = 2
EV_CURRENT = 1

PT_LOAD = 1
PF_X, PF_W, PF_R = 0x1, 0x2, 0x4

SHT_NULL, SHT_PROGBITS, SHT_NOBITS, SHT_STRTAB = 0, 1, 8, 3
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 0x1, 0x2, 0x4

# EF_MIPS_ARCH_5900 | EABI64, matching the main SLUS_212.07 executable so the
# same Ghidra processor selection and ps2_recomp path apply to both.
E_FLAGS_R5900_EABI64 = 0x20924001

OVERLAY_NAMES = {
    1: "casino",
    2: "viewer",
    3: "shop",
    4: "menu",
    5: "battle",
    6: "title",
}


@dataclass(frozen=True)
class Mwo3Header:
    overlay_id: int
    load_address: int
    text_size: int
    data_size: int
    bss_size: int
    ctor_start: int
    ctor_end: int
    name: str

    @property
    def text_address(self) -> int:
        return self.load_address + HEADER_SIZE

    @property
    def data_address(self) -> int:
        return self.text_address + self.text_size

    @property
    def bss_address(self) -> int:
        return self.data_address + self.data_size

    @property
    def end_address(self) -> int:
        return self.bss_address + self.bss_size

    @property
    def slug(self) -> str:
        """Stable short name: the header's own name wins, id table is fallback."""
        stem = self.name.split(".")[0].strip().lower()
        return stem or OVERLAY_NAMES.get(self.overlay_id, f"ovl{self.overlay_id}")


def parse_header(blob: bytes, source: str) -> Mwo3Header:
    if len(blob) < HEADER_SIZE:
        raise ValueError(f"{source}: too small to be an MWo3 overlay ({len(blob)} bytes)")
    if blob[:4] != MWO3_MAGIC:
        raise ValueError(f"{source}: not an MWo3 overlay (magic {blob[:4]!r})")

    fields = struct.unpack_from("<7I", blob, 4)
    name = blob[0x20:HEADER_SIZE].split(b"\0", 1)[0].decode("ascii", "replace")
    header = Mwo3Header(*fields, name=name)

    expected = HEADER_SIZE + header.text_size + header.data_size
    if expected != len(blob):
        raise ValueError(
            f"{source}: header describes {expected} bytes "
            f"(0x40 + text 0x{header.text_size:x} + data 0x{header.data_size:x}) "
            f"but the file is {len(blob)}. Refusing to guess."
        )
    if header.text_size % 4:
        raise ValueError(f"{source}: .text size 0x{header.text_size:x} is not word-aligned")
    if header.ctor_end < header.ctor_start:
        raise ValueError(f"{source}: ctor range runs backwards")
    if header.ctor_start and not (
        header.data_address <= header.ctor_start <= header.ctor_end <= header.bss_address
    ):
        # Not fatal -- report it and carry on, since the ctor table is metadata
        # for the runtime rather than something this conversion depends on.
        print(
            f"warning: {source}: ctor range 0x{header.ctor_start:08x}-0x{header.ctor_end:08x} "
            f"lies outside .data (0x{header.data_address:08x}-0x{header.bss_address:08x})",
            file=sys.stderr,
        )
    return header


def describe(header: Mwo3Header, source: str) -> str:
    ctors = (header.ctor_end - header.ctor_start) // 4
    return (
        f"{source}\n"
        f"  id           {header.overlay_id} ({header.slug})\n"
        f"  name         {header.name}\n"
        f"  load         0x{header.load_address:08x}\n"
        f"  .text        0x{header.text_address:08x} + 0x{header.text_size:06x}\n"
        f"  .data        0x{header.data_address:08x} + 0x{header.data_size:06x}\n"
        f"  .bss         0x{header.bss_address:08x} + 0x{header.bss_size:06x}\n"
        f"  arena end    0x{header.end_address:08x}\n"
        f"  ctors        0x{header.ctor_start:08x}-0x{header.ctor_end:08x} ({ctors} entries)"
    )


def build_elf(header: Mwo3Header, blob: bytes) -> bytes:
    """Emit a 32-bit little-endian ET_EXEC MIPS ELF holding the overlay image.

    Both program headers and section headers are written. `ps2_recomp` reads
    PT_LOAD segments, while Ghidra and objdump give much better output from
    real section headers, and the two describe the same bytes.
    """
    text = blob[HEADER_SIZE : HEADER_SIZE + header.text_size]
    data = blob[HEADER_SIZE + header.text_size :]

    ehsize, phentsize, shentsize = 52, 32, 40
    phnum = 3 if header.bss_size else 2
    # .null .text .data .bss .shstrtab -- .bss only when it exists.
    section_names = [""] + [".text", ".data"] + ([".bss"] if header.bss_size else []) + [".shstrtab"]
    shnum = len(section_names)

    shstrtab = b"\0".join(name.encode() for name in section_names) + b"\0"
    name_offsets, cursor = {}, 0
    for name in section_names:
        name_offsets[name] = cursor
        cursor += len(name) + 1

    phoff = ehsize
    text_off = phoff + phnum * phentsize
    # Keep .text 16-byte aligned in the file purely for readability in hex dumps.
    text_off = (text_off + 15) & ~15
    data_off = text_off + len(text)
    shstrtab_off = data_off + len(data)
    shoff = (shstrtab_off + len(shstrtab) + 3) & ~3

    out = bytearray()
    out += struct.pack(
        "<4sBBBBB7sHHIIIIIHHHHHH",
        b"\x7fELF", 1, 1, EV_CURRENT, 0, 0, b"\0" * 7,
        ET_EXEC, EM_MIPS, EV_CURRENT,
        header.text_address,          # e_entry -- first instruction of the overlay
        phoff, shoff, E_FLAGS_R5900_EABI64,
        ehsize, phentsize, phnum, shentsize, shnum, shnum - 1,
    )

    def program_header(p_offset, p_vaddr, p_filesz, p_memsz, p_flags):
        return struct.pack(
            "<8I", PT_LOAD, p_offset, p_vaddr, p_vaddr, p_filesz, p_memsz, p_flags, 16
        )

    out += program_header(text_off, header.text_address, len(text), len(text), PF_R | PF_X)
    # .data is mapped R+W+X, matching the main executable's single WAX PROGBITS.
    # This is not cosmetic: Metrowerks puts the C++ static-constructor thunks that
    # `ctor_begin..ctor_end` points at *inside .data* (verified for all six US
    # overlays), and ps2_recomp's Ghidra-map loader drops any function whose
    # containing section lacks SHF_EXECINSTR. Marked R+W only, every overlay's
    # constructors would be silently untranslated.
    out += program_header(data_off, header.data_address, len(data), len(data), PF_R | PF_W | PF_X)
    if header.bss_size:
        # filesz 0, memsz non-zero: the loader zero-fills it, same as the game's
        # own overlay loader does.
        out += program_header(shstrtab_off, header.bss_address, 0, header.bss_size, PF_R | PF_W)

    out += b"\0" * (text_off - len(out))
    out += text
    out += data
    out += shstrtab
    out += b"\0" * (shoff - len(out))

    def section_header(name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_align):
        return struct.pack(
            "<10I", name_offsets[name], sh_type, sh_flags, sh_addr,
            sh_offset, sh_size, 0, 0, sh_align, 0,
        )

    out += section_header("", SHT_NULL, 0, 0, 0, 0, 0)
    out += section_header(
        ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
        header.text_address, text_off, len(text), 16,
    )
    out += section_header(
        ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR,
        header.data_address, data_off, len(data), 16,
    )
    if header.bss_size:
        out += section_header(
            ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
            header.bss_address, shstrtab_off, header.bss_size, 16,
        )
    out += section_header(".shstrtab", SHT_STRTAB, 0, 0, shstrtab_off, len(shstrtab), 1)

    return bytes(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Wrap a Metrowerks MWo3 overlay in a minimal MIPS ELF.",
        epilog="With --info, headers are decoded and printed and nothing is written.",
    )
    parser.add_argument("inputs", nargs="+", type=Path, help="overlay .BIN file(s)")
    parser.add_argument("-o", "--output", type=Path,
                        help="output ELF (single input only; default <slug>.elf beside --outdir)")
    parser.add_argument("-d", "--outdir", type=Path,
                        help="write <slug>.elf into this directory")
    parser.add_argument("--info", action="store_true",
                        help="decode and print headers without writing anything")
    args = parser.parse_args(argv)

    if args.output and len(args.inputs) > 1:
        parser.error("--output takes a single input; use --outdir for several")
    if not args.info and not args.output and not args.outdir:
        parser.error("need --info, --output, or --outdir")

    status = 0
    for path in args.inputs:
        try:
            blob = path.read_bytes()
            header = parse_header(blob, str(path))
        except (OSError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            status = 1
            continue

        if args.info:
            print(describe(header, str(path)))
            continue

        target = args.output or (args.outdir / f"{header.slug}.elf")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(build_elf(header, blob))
        print(
            f"{path} -> {target}  "
            f"id={header.overlay_id} {header.slug} "
            f".text 0x{header.text_address:08x}+0x{header.text_size:x}"
        )

    return status


if __name__ == "__main__":
    sys.exit(main())
