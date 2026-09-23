#!/usr/bin/env python3
"""Reject known local-only payloads and common credentials in tracked files.

Checks the current checkout, not Git history or the contents of submodules.
This is a small accidental-inclusion guard, not a license or secret audit.
"""

from __future__ import annotations

import argparse
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


PAYLOAD_SUFFIXES = (
    ".iso", ".cso", ".chd", ".elf", ".bin", ".dat", ".hd6", ".mvi",
    ".pss", ".gs", ".gs.xz", ".gs.zst", ".raw32", ".state", ".ps2",
    ".p2s", ".mwo3", ".sav",
)
PRIVATE_DIRECTORIES = {"build", "generated", "gsdumps", "captures", "memcards", "assets", "extracted"}
SECRET = re.compile(
    rb"-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----"
    rb"|gh[pousr]_[A-Za-z0-9]{36,}"
    rb"|github_pat_[A-Za-z0-9_]{60,}"
    rb"|AKIA[0-9A-Z]{16}"
)


def path_violation(name: str) -> str | None:
    path = PurePosixPath(name.lower())
    if any(part in PRIVATE_DIRECTORIES or part.startswith(("assets_", "extracted_"))
           for part in path.parts):
        return "local game/build directory"
    if path.name.endswith(PAYLOAD_SUFFIXES) or re.fullmatch(r"s[lcp][a-z]{2}_\d{3}\.\d{2}", path.name):
        return "game binary, archive, or capture"
    if re.fullmatch(r"(?:fun|sub|recomp_func)_[0-9a-f]+(?:_0x[0-9a-f]+)?\.cpp", path.name):
        return "generated game function"
    if (path.name.endswith((".key", ".pem", ".p12", ".pfx"))
            or path.name in {"id_rsa", "id_ed25519", "credentials.json"}
            or (path.name.startswith(".env") and path.name not in {".env.example", ".env.template"})):
        return "key or credential file"
    return None


def check_checkout(root: Path) -> list[tuple[str, str]]:
    tracked = subprocess.check_output(["git", "-C", str(root), "ls-files", "--stage", "-z"])
    failures = []
    for entry in tracked.split(b"\0"):
        if not entry:
            continue
        metadata, encoded_name = entry.split(b"\t", 1)
        if metadata.startswith(b"160000 "):
            continue  # Submodule fixtures are owned and reviewed upstream.
        name = encoded_name.decode("utf-8", errors="surrogateescape")
        reason = path_violation(name)
        file = root / name
        if reason is None and file.is_file() and not file.is_symlink():
            if SECRET.search(file.read_bytes()):
                reason = "credential signature"
        if reason:
            failures.append((name, reason))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    failures = check_checkout(args.root)
    for name, reason in failures:
        print(f"source distribution: {name}: {reason}", file=sys.stderr)
    if failures:
        return 1
    print("source distribution: tracked files passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
