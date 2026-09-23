#!/usr/bin/env python3
"""HD6 archive extractor for Dragon Quest VIII (PS2, PAL / SLES-539.74).

Extracts the contents of DATA.DAT / DATA2.DAT using the accompanying
DATA.HD6 / DATA2.HD6 index files.

File-table bit layout (RESOLVED EMPIRICALLY against the PAL discs):

    Each file-table entry is a little-endian u64, packed as:

        bits  0..16  (17 bits) : byte offset of this entry's record in the
                                 name table
        bits 17..37  (21 bits) : data offset in the .DAT, in 1024-byte units
        bits 38..63  (26 bits) : data size, in 4-byte units

    Two additional invariants hold for every entry in both PAL archives
    (10632 + 4140 entries):

        * bit 17 is always 0  -> offsets are 2048-aligned (DVD sector size)
        * size %  4 units == 0 -> sizes are always multiples of 16 bytes

    So the layout can equivalently be read as a 20-bit sector number
    (bits 18..37, x2048) and a 24-bit size (bits 40..63, x16).

    This resolves the two competing descriptions of the format:
      - "17-bit name / 20-bit sector@17..36 x2048 / 27-bit size@37..63 x1"
        is WRONG: both field boundaries are off by one bit.  It appears to
        work for the first ~7100 entries of DATA.HD6 because the name
        offsets (unchanged) are monotonic and offsets below 1 GiB with
        16-aligned sizes happen to decode to half-offset/half-size values
        that stay self-consistent; entries past 1 GiB overflow the
        misplaced 20-bit field and decode to garbage (3606 apparent
        overlaps).
      - Fire-Cube's "(u24@bytes2..4 & 0xFFFFFC) << 9" offset and
        "u24@bytes5..7 << 4" size (Dragon-Quest-8-Tools/HD6Tools.py) are
        arithmetically identical to the layout above given the two
        invariants, and are CORRECT.

    Verification on this data: with the 17/21/26 layout, sorting all
    entries by offset yields zero overlaps, every inter-file gap is
    < 2048 bytes (pure sector padding), the last entry ends within one
    sector of the end of the .DAT, and the sum of sizes covers 99.4 % of
    each .DAT.  Known file headers (ICO, RIFF/WAVE, IM3, plain text)
    appear exactly at the decoded offsets.

Header (0x34 bytes, all little-endian u32 after the magic):

    +0x00  char[4]  magic            "HD6\\0"
    +0x04  u32      token_pool_off   == 0x34
    +0x08  u32      token_pool_size
    +0x0C  u32      token_count      (token 0 is the empty string)
    +0x10  u32      reserved         0
    +0x14  u32      name_table_off   == token_pool_off + token_pool_size
    +0x18  u32      name_table_size
    +0x1C  u32      reserved         0
    +0x20  u32      unk_10           0x10
    +0x24  u32      file_count       (includes one trailing sentinel entry)
    +0x28  u32      file_table_off
    +0x2C  u32      reserved         0
    +0x30  u32      total_size       == size of the .HD6 file

Token pool: NUL-terminated ASCII fragments, exactly token_count of them,
back to back.  Fragments embed '\\' path separators (e.g. "map\\c01").

Name table: one record per file entry, addressed by the 17-bit name
offset.  A record is a sequence of LEB128 varints (little-endian base-128,
MSB = continuation) indexing into the token pool, terminated by a single
0x00 byte.  A raw 0x00 is always a record terminator, never a reference to
token 0: token 0 is the empty string and is never referenced (its
canonical encoding would be the terminator byte itself).  The filename is
the concatenation of the referenced tokens.

The final file-table entry is a sentinel: offset 0, size 0, empty name.

Dummy entries: entries whose data region is 0xFD fill are placeholder
assets (the disc ships a 51 MB DMYDATA pad file).  Detection: first
min(64, size) bytes all 0xFD.  Neither PAL archive actually contains any,
but the check is kept for other revisions.  Dummies are recorded in the
manifest with "dummy": true and are not written to disk.

Note on sizes: the stored size is the 16-byte-aligned on-disc size.  For
files whose true length is not a multiple of 16 (e.g. RIFF/WAVE audio)
the extracted file carries up to 15 trailing padding bytes; the archive
does not store the exact length.

CLI:
    python3 hd6_extract.py --hd6 <file.HD6> --dat <file.DAT> \
        --out <dir> --manifest <manifest.json> [--list-only]
"""

import argparse
import hashlib
import json
import os
import struct
import sys

HEADER_FMT = "<4s12I"
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 0x34
MAGIC = b"HD6\x00"

NAME_OFF_BITS = 17
OFF_BITS = 21
OFF_UNIT = 1024
SIZE_UNIT = 4


class Hd6Error(Exception):
    pass


def parse_hd6(hd6_bytes):
    """Parse an HD6 index.  Returns (tokens, entries) where each entry is a
    dict {index, name, name_off, offset, size, sentinel}."""
    if len(hd6_bytes) < HEADER_SIZE:
        raise Hd6Error("file too small for HD6 header")
    (magic, tok_off, tok_size, tok_count, res1,
     name_off, name_size, res2,
     unk10, file_count, ftab_off, res3, total_size) = struct.unpack_from(
        HEADER_FMT, hd6_bytes, 0)
    if magic != MAGIC:
        raise Hd6Error("bad magic %r" % magic)
    if total_size != len(hd6_bytes):
        raise Hd6Error("header total_size %d != actual file size %d"
                       % (total_size, len(hd6_bytes)))
    if name_off != tok_off + tok_size:
        raise Hd6Error("name_table_off %#x != token_pool end %#x"
                       % (name_off, tok_off + tok_size))
    if ftab_off + 8 * file_count > len(hd6_bytes):
        raise Hd6Error("file table extends past end of file")

    # --- token pool ---
    pool = hd6_bytes[tok_off:tok_off + tok_size]
    tokens = []
    pos = 0
    for _ in range(tok_count):
        end = pool.find(b"\x00", pos)
        if end < 0:
            raise Hd6Error("unterminated token %d" % len(tokens))
        tokens.append(pool[pos:end].decode("ascii"))
        pos = end + 1
    if tokens and tokens[0] != "":
        raise Hd6Error("token 0 is %r, expected empty string" % tokens[0])

    # --- name table ---
    ntab = hd6_bytes[name_off:name_off + name_size]

    def read_name(off):
        if off >= len(ntab):
            raise Hd6Error("name offset %d past name table (%d)"
                           % (off, len(ntab)))
        parts = []
        i = off
        while ntab[i] != 0x00:  # 0x00 terminates the record (never token 0)
            value = 0
            shift = 0
            while True:
                b = ntab[i]
                i += 1
                value |= (b & 0x7F) << shift
                if not (b & 0x80):
                    break
                shift += 7
            if value >= len(tokens):
                raise Hd6Error("token index %d out of range at name off %d"
                               % (value, off))
            parts.append(tokens[value])
        return "".join(parts)

    # --- file table ---
    entries = []
    for k in range(file_count):
        (q,) = struct.unpack_from("<Q", hd6_bytes, ftab_off + 8 * k)
        noff = q & ((1 << NAME_OFF_BITS) - 1)
        offset = ((q >> NAME_OFF_BITS) & ((1 << OFF_BITS) - 1)) * OFF_UNIT
        size = (q >> (NAME_OFF_BITS + OFF_BITS)) * SIZE_UNIT
        name = read_name(noff)
        sentinel = (offset == 0 and size == 0 and name == "")
        entries.append({
            "index": k,
            "name": name,
            "name_off": noff,
            "offset": offset,
            "size": size,
            "sentinel": sentinel,
        })
    return tokens, entries


def sanitize_name(name):
    """Convert an archive name ('bin\\actinfo.txt') to a safe relative
    POSIX path.  Raises Hd6Error on traversal attempts."""
    rel = name.replace("\\", "/")
    if rel.startswith("/") or (len(rel) > 1 and rel[1] == ":"):
        raise Hd6Error("absolute path in archive name: %r" % name)
    parts = [p for p in rel.split("/") if p not in ("", ".")]
    if not parts:
        raise Hd6Error("empty archive name")
    for part in parts:
        if part == ".." or any(ord(c) < 0x20 or c == "\x7f" for c in part):
            raise Hd6Error("unsafe archive name: %r" % name)
    return "/".join(parts)


def extract(hd6_path, dat_path, out_dir, manifest_path, list_only=False):
    with open(hd6_path, "rb") as f:
        hd6_bytes = f.read()
    tokens, entries = parse_hd6(hd6_bytes)

    if list_only:
        for e in entries:
            if not e["sentinel"]:
                print(e["name"].replace("\\", "/"))
        return 0

    dat_size = os.path.getsize(dat_path)
    manifest_entries = []
    seen_names = {}
    n_written = 0
    n_dummy = 0
    total_bytes = 0

    with open(dat_path, "rb") as dat:
        for e in entries:
            if e["sentinel"]:
                continue
            rel = sanitize_name(e["name"])
            if e["offset"] + e["size"] > dat_size:
                raise Hd6Error("entry %d %r [%#x..%#x) extends past end of "
                               "%s" % (e["index"], rel, e["offset"],
                                       e["offset"] + e["size"], dat_path))
            dat.seek(e["offset"])
            data = dat.read(e["size"])
            if len(data) != e["size"]:
                raise Hd6Error("short read for entry %d %r"
                               % (e["index"], rel))

            head = data[:64]
            dummy = e["size"] > 0 and head == b"\xfd" * len(head)

            record = {
                "index": e["index"],
                "name": rel,
                "sector": e["offset"] // 2048,
                "offset_bytes": e["offset"],
                "size": e["size"],
                "dummy": dummy,
            }
            if dummy:
                n_dummy += 1
            else:
                record["sha1"] = hashlib.sha1(data).hexdigest()
                dest = os.path.join(out_dir, rel)
                if rel in seen_names:
                    prev = seen_names[rel]
                    note = ("identical" if prev == record["sha1"]
                            else "DIFFERENT CONTENT")
                    print("warning: duplicate name %r (%s); keeping last"
                          % (rel, note), file=sys.stderr)
                seen_names[rel] = record["sha1"]
                os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
                with open(dest, "wb") as out:
                    out.write(data)
                n_written += 1
                total_bytes += e["size"]
            manifest_entries.append(record)

    manifest = {
        "hd6": os.path.basename(hd6_path),
        "dat": os.path.basename(dat_path),
        "format": "name:17 bits | offset:21 bits x1024 | size:26 bits x4",
        "entry_count": len(entries),
        "sentinel_count": sum(1 for e in entries if e["sentinel"]),
        "written": n_written,
        "dummies": n_dummy,
        "total_bytes": total_bytes,
        "entries": manifest_entries,
    }
    os.makedirs(os.path.dirname(os.path.abspath(manifest_path)),
                exist_ok=True)
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")

    print("%s: %d entries -> wrote %d files (%d bytes), %d dummies, "
          "%d sentinel" % (os.path.basename(hd6_path), len(entries),
                           n_written, total_bytes, n_dummy,
                           manifest["sentinel_count"]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Extract a DQ8 .DAT archive using its .HD6 index")
    ap.add_argument("--hd6", required=True, help="path to the .HD6 index")
    ap.add_argument("--dat", help="path to the .DAT archive "
                                  "(required unless --list-only)")
    ap.add_argument("--out", help="output directory")
    ap.add_argument("--manifest", help="manifest JSON output path")
    ap.add_argument("--list-only", action="store_true",
                    help="print archive names and exit")
    args = ap.parse_args(argv)

    if not args.list_only:
        missing = [n for n in ("dat", "out", "manifest")
                   if getattr(args, n) is None]
        if missing:
            ap.error("missing required arguments: %s"
                     % ", ".join("--" + m for m in missing))

    try:
        return extract(args.hd6, args.dat, args.out, args.manifest,
                       list_only=args.list_only)
    except Hd6Error as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
