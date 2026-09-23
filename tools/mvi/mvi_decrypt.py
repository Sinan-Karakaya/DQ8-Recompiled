#!/usr/bin/env python3
"""Convert local MVI movies to PSS using a user-supplied XOR key."""

import argparse
import os
import sys

BLOCK = 0x4000
TRAILER = 4


def load_key(path):
    with open(path, "rb") as f:
        key = f.read()
    if not key:
        raise ValueError("empty key file: %s" % path)
    return key


def decrypt_bytes(data, key):
    """Return decrypted body (trailer stripped)."""
    body_len = len(data) - TRAILER
    if body_len < 0 or body_len % BLOCK != 0:
        raise ValueError("bad MVI length %d (expected N*%#x + %d)"
                         % (len(data), BLOCK, TRAILER))
    body = bytearray(data[:body_len])
    klen = len(key)
    # Bulk integer XOR avoids a Python loop over every byte.
    kbuf = (key * (body_len // klen + 1))[:body_len]
    out = (int.from_bytes(body, "big") ^ int.from_bytes(kbuf, "big"))
    return out.to_bytes(body_len, "big")


def find_mvis(root):
    for dirpath, _dirs, files in os.walk(root):
        for name in sorted(files):
            if name.upper().endswith(".MVI"):
                yield os.path.join(dirpath, name)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Decrypt DQ8 .MVI -> .pss")
    ap.add_argument("--in", dest="indir", required=True,
                    help="directory to scan recursively for .MVI files")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--key", required=True,
                    help="path to a locally supplied keystream; no key is distributed")
    args = ap.parse_args(argv)

    key = load_key(args.key)
    root = os.path.abspath(args.indir)
    files = list(find_mvis(root))
    if not files:
        print("no .MVI files found under %s" % root, file=sys.stderr)
        return 1

    total = 0
    for path in files:
        rel = os.path.relpath(path, root)
        stem = rel[:-4] if rel.upper().endswith(".MVI") else rel
        dest = os.path.join(args.out, stem + ".pss")
        with open(path, "rb") as f:
            data = f.read()
        try:
            dec = decrypt_bytes(data, key)
        except ValueError as exc:
            print("skip %s: %s" % (rel, exc), file=sys.stderr)
            continue
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        with open(dest, "wb") as f:
            f.write(dec)
        ok = dec[:4] == b"\x00\x00\x01\xba"
        total += 1
        print("%-28s -> %s  (%d bytes)%s"
              % (rel, os.path.relpath(dest), len(dec),
                 "" if ok else "  [WARN: no pack header]"))
    print("decrypted %d/%d MVI files" % (total, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
