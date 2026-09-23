#!/usr/bin/env python3
"""DQ8Recomp setup tool.

Verifies the user's disc dump, extracts assets from an existing disc tree,
and drives the static recompiler. Assets and generated C++ stay local.

Subcommands:
    verify      Check the ISO and extracted files against known-good hashes.
    extract     Unpack HD6 archives and decrypt MVI movies from a disc tree.
    recompile   Drive ps2xRecomp over the ELF + overlays.

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
CONFIG_DIR = REPO_ROOT / "config"

# Default asset output lives next to the repo checkout, never inside it.
DEFAULT_ASSETS_DIR = REPO_ROOT.parent / "Assets"

DEFAULT_VERSION = "SLUS_212.07"
EXTRACTED_DIRS = {"SLUS_212.07": "Extracted_Usa", "SLES_539.74": "Extracted_Eur"}

CHUNK_SIZE = 4 * 1024 * 1024


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def sha256_file(path: Path, label: str | None = None) -> str:
    """Chunked sha256 with a lightweight progress display for large files."""
    total = path.stat().st_size
    done = 0
    show_progress = total > 256 * 1024 * 1024 and sys.stderr.isatty()
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
            done += len(chunk)
            if show_progress:
                pct = 100.0 * done / total
                print(f"\r  hashing {label or path.name}: {pct:5.1f}%",
                      end="", file=sys.stderr, flush=True)
    if show_progress:
        print("", file=sys.stderr)
    return digest.hexdigest()


def load_hashes(version: str) -> dict:
    hashes_path = CONFIG_DIR / version / "hashes.json"
    if not hashes_path.is_file():
        sys.exit(f"error: no hash database for version '{version}' "
                 f"(expected {hashes_path})")
    with hashes_path.open(encoding="utf-8") as fh:
        return json.load(fh)


def check_file(path: Path, expected: dict, rel_name: str) -> bool:
    """Compare one file against {'sha256': ..., 'size': ...}. Returns ok."""
    if not path.is_file():
        print(f"  MISSING  {rel_name}")
        return False
    size = path.stat().st_size
    if size != expected["size"]:
        print(f"  SIZE     {rel_name}: {size} != expected {expected['size']}")
        return False
    actual = sha256_file(path, rel_name)
    if actual != expected["sha256"]:
        print(f"  HASH     {rel_name}: {actual}")
        print(f"           expected: {expected['sha256']}")
        return False
    print(f"  OK       {rel_name}")
    return True


def run_tool(argv: list[str], what: str) -> None:
    print(f"[extract] {what}: {' '.join(str(a) for a in argv)}")
    result = subprocess.run(argv)
    if result.returncode != 0:
        sys.exit(f"error: {what} failed with exit code {result.returncode}")


# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------

def cmd_verify(args: argparse.Namespace) -> None:
    db = load_hashes(args.version)
    iso_path = Path(args.iso)
    ok = True

    print(f"Verifying against config/{args.version}/hashes.json")

    # 1. The ISO itself.
    print("ISO:")
    ok &= check_file(iso_path, db["iso"], iso_path.name)

    # 2. The ELF (and optionally everything else) in the extracted tree.
    extracted = None
    if args.extracted:
        extracted = Path(args.extracted)
    else:
        directory = EXTRACTED_DIRS.get(args.version)
        if directory:
            candidate = iso_path.parent / directory
            if candidate.is_dir():
                extracted = candidate

    if extracted is None:
        print("note: no extracted tree found (pass --extracted DIR); "
              "skipping ELF verification")
    else:
        print(f"Extracted tree ({extracted}):")
        files: dict = db.get("files", {})
        if args.all:
            names = list(files)
        else:
            names = [db["elf"]]
        for rel in names:
            ok &= check_file(extracted / rel, files[rel], rel)

    if not ok:
        sys.exit("verify: FAILED — this dump does not match the supported "
                 f"{args.version} image")
    print("verify: OK")


# ---------------------------------------------------------------------------
# extract
# ---------------------------------------------------------------------------

def cmd_extract(args: argparse.Namespace) -> None:
    iso_path = Path(args.iso)
    out_dir = Path(args.out)

    if args.extracted:
        extracted = Path(args.extracted)
        if not extracted.is_dir():
            sys.exit(f"error: --extracted {extracted} is not a directory")
        print(f"[extract] using pre-extracted ISO tree: {extracted}")
    else:
        sys.exit(
            "error: an already-extracted disc tree is required.\n"
            "Extract the disc image with your tool of choice, then re-run:\n"
            f"  {sys.argv[0]} extract --iso {iso_path} --extracted <DIR>"
        )

    # Validate user-supplied inputs before writing a partial asset tree.
    movie_dir = extracted / "MOVIE"
    movie_key = Path(args.mvi_key) if args.mvi_key else None
    if movie_dir.is_dir():
        if movie_key is None:
            sys.exit("error: MOVIE is present; pass --mvi-key PATH to your own "
                     "2048-byte MVI key (no key is distributed with this project)")
        if not movie_key.is_file() or movie_key.stat().st_size != 2048:
            sys.exit(f"error: --mvi-key {movie_key} must be a 2048-byte file")

    archives = (("DATA.HD6", "DATA.DAT", "data"),
                ("DATA2.HD6", "DATA2.DAT", "data2"))
    for hd6_name, dat_name, _ in archives:
        if not (extracted / hd6_name).is_file() or not (extracted / dat_name).is_file():
            sys.exit(f"error: {hd6_name}/{dat_name} not found in {extracted}")

    manifest_dir = out_dir / "manifests"
    manifest_dir.mkdir(parents=True, exist_ok=True)

    # (b) HD6 archive extraction (tools/hd6 contract; see tools/hd6/README.md).
    for hd6_name, dat_name, sub in archives:
        hd6 = extracted / hd6_name
        dat = extracted / dat_name
        run_tool(
            [sys.executable, str(REPO_ROOT / "tools" / "hd6" / "hd6_extract.py"),
             "--hd6", str(hd6),
             "--dat", str(dat),
             "--out", str(out_dir / sub),
             "--manifest", str(manifest_dir / f"{hd6_name}.manifest.json")],
            f"extracting {hd6_name}/{dat_name}",
        )

    # (c) MVI movie decryption (tools/mvi contract; see tools/mvi/README.md).
    if movie_dir.is_dir():
        run_tool(
            [sys.executable, str(REPO_ROOT / "tools" / "mvi" / "mvi_decrypt.py"),
             "--in", str(movie_dir),
             "--key", str(movie_key),
             "--out", str(out_dir / "movies")],
            "decrypting MVI movies",
        )
    else:
        print(f"warning: {movie_dir} not found; skipping movie decryption")

    print(f"extract: done -> {out_dir}")


# ---------------------------------------------------------------------------
# recompile
# ---------------------------------------------------------------------------

def cmd_recompile(args: argparse.Namespace) -> None:
    if args.version != "SLUS_212.07":
        sys.exit("recompile: the complete runtime function maps currently target SLUS_212.07")
    extracted = Path(args.extracted).resolve()
    recompiler = Path(args.recompiler).resolve()
    config_dir = CONFIG_DIR / args.version
    overlays = ("title", "casino", "viewer", "battle", "menu", "shop")
    required = [recompiler, extracted / args.version,
                *(extracted / "BIN" / f"{name.upper()}.BIN" for name in overlays)]
    for path in required:
        if not path.is_file():
            sys.exit(f"recompile: missing {path}")

    build = REPO_ROOT / "build"
    local_configs = build / "recompile-configs" / args.version
    local_configs.mkdir(parents=True, exist_ok=True)

    def translate(template: Path, name: str, source: Path, csv: Path, output: Path) -> None:
        text = template.read_text(encoding="utf-8")
        for key, path in (("input", source), ("ghidra_output", csv), ("output", output)):
            text, count = re.subn(rf'^{key}\s*=.*$',
                                 lambda _: f"{key} = {json.dumps(str(path))}",
                                 text, flags=re.MULTILINE)
            if count != 1:
                sys.exit(f"recompile: expected one {key} in {template}")
        config = local_configs / f"{name}.toml"
        config.write_text(text, encoding="utf-8")
        subprocess.run([str(recompiler), str(config)], cwd=REPO_ROOT, check=True)

    translate(config_dir / "dq8.toml", "main", extracted / args.version,
              config_dir / "functions.enriched.csv", build / "generated" / args.version)
    subprocess.run([sys.executable, str(REPO_ROOT / "tools/mwo3/mwo3_to_elf.py"),
                    "-d", str(build / "overlays"),
                    *(str(extracted / "BIN" / f"{name.upper()}.BIN") for name in overlays)],
                   check=True)
    overlay_output = build / "generated" / f"{args.version}-overlays"
    for name in overlays:
        translate(config_dir / "overlays" / f"{name}.toml", name,
                  build / "overlays" / f"{name}.elf",
                  config_dir / f"functions_{name}.csv", overlay_output / name)
    subprocess.run([sys.executable, str(REPO_ROOT / "tools/mwo3/gen_overlay_table.py"),
                    "--all", str(overlay_output)], check=True)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        prog="setup.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_verify = sub.add_parser("verify", help="verify a disc dump against known hashes")
    p_verify.add_argument("--iso", required=True, help="path to the disc image")
    p_verify.add_argument("--version", default=DEFAULT_VERSION,
                          help=f"game version id (default: {DEFAULT_VERSION})")
    p_verify.add_argument("--extracted",
                          help="extracted ISO tree (auto-detects sibling Extracted_Usa/Extracted_Eur by version)")
    p_verify.add_argument("--all", action="store_true",
                          help="verify every file in the hash database, not just the ELF")
    p_verify.set_defaults(func=cmd_verify)

    p_extract = sub.add_parser("extract", help="extract assets from an extracted disc tree")
    p_extract.add_argument("--iso", required=True, help="path to the disc image")
    p_extract.add_argument("--out", default=str(DEFAULT_ASSETS_DIR),
                           help=f"asset output directory (default: {DEFAULT_ASSETS_DIR})")
    p_extract.add_argument("--extracted",
                           help="pre-extracted ISO tree (required)")
    p_extract.add_argument("--mvi-key", help="user-supplied 2048-byte MVI key (required when MOVIE is present)")
    p_extract.set_defaults(func=cmd_extract)

    p_recompile = sub.add_parser("recompile", help="recompile the ELF + overlays to C++")
    p_recompile.add_argument("--version", default="SLUS_212.07")
    p_recompile.add_argument("--extracted", required=True, help="extracted NTSC-U disc tree")
    p_recompile.add_argument("--recompiler", default=str(
        REPO_ROOT / "build/ps2recomp-standalone/ps2xRecomp/ps2_recomp"))
    p_recompile.set_defaults(func=cmd_recompile)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
