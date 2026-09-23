#!/usr/bin/env python3
"""Turn a per-overlay `register_functions.cpp` into a namespaced dispatch table.

`ps2_recomp` has no notion of overlays. Run over an MWo3 overlay ELF it emits,
exactly as it does for the main executable, a `register_functions.cpp` that
DEFINES the four globals `ps2xRuntime` declares once for the whole program:

    g_ps2RecompiledFunctionTableBase / End / SlotCount
    g_ps2RecompiledFunctionTable[]

Six overlays plus the main executable would therefore define the same four
symbols seven times, and the six overlay tables all start at 0x00461CC0
because the images overlap in the guest's arena by design. The per-function
symbols do NOT collide -- every overlay's Ghidra map is prefixed
(`FUN_TITLE_text__*`, `title_overlay_entry`) and every generated name carries
its address -- so the ONLY thing standing between the six overlays and a clean
link is this one file.

So: drop the generated `register_functions.cpp` from the build and run this
script over it instead. It re-emits the identical slot assignments as

    namespace ovl_<slug> {
        PS2Runtime::FunctionRegion &functionRegion();   // lazily filled
    }

which the DQ8 overlay manager (src/runtime/overlay) hands to
`PS2Runtime::setFunctionRegionResolver` whenever that overlay is the resident
one. Filling is done inside a function-local static, so there is no
static-initialisation-order dependency on the generated per-function TUs and
no cost for overlays the session never loads.

Usage:
    gen_overlay_table.py build/generated/SLUS_212.07-overlays/title \\
        --slug title -o .../title/overlay_table_title.cpp
    gen_overlay_table.py --all build/generated/SLUS_212.07-overlays
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

EXTERN_RE = re.compile(
    r"^extern const uint32_t g_ps2RecompiledFunctionTable(Base|End|SlotCount) = ([0-9a-fA-Fxu]+);",
    re.MULTILINE,
)
ENTRY_RE = re.compile(
    r"^\s*g_ps2RecompiledFunctionTable\[(\d+)\]\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;\s*(//.*)?$"
)

HEADER = """// GENERATED FILE -- do not edit. Regenerate with:
//   python3 tools/mwo3/gen_overlay_table.py --all build/generated/SLUS_212.07-overlays
//
// Source: {source}
//
// This replaces ps2_recomp's own register_functions.cpp for this overlay. See
// tools/mwo3/gen_overlay_table.py and https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Architecture.
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"

namespace ovl_{slug}
{{
namespace
{{
// Guest addresses covered by this overlay image, from the MWo3 header:
// .text starts at load_addr + 0x40 and the last entry is the highest
// translated function start (static constructors live in .data, which
// mwo3_to_elf.py marks executable so they are translated too).
constexpr uint32_t kBase = {base}u;
constexpr uint32_t kEnd = {end}u;
constexpr uint32_t kSlotCount = {slots}u;

PS2Runtime::RecompiledFunction g_slots[kSlotCount] = {{}};

PS2Runtime::FunctionRegion buildRegion()
{{
"""

FOOTER = """    return PS2Runtime::FunctionRegion{{kBase, kEnd, kSlotCount, g_slots}};
}}
}} // namespace

// Filled on first use: the overlay manager only asks for this once the game
// has actually loaded {slug}.bin, which is long after static init.
PS2Runtime::FunctionRegion &functionRegion()
{{
    static PS2Runtime::FunctionRegion region = buildRegion();
    return region;
}}

}} // namespace ovl_{slug}
"""


def convert(source: Path, slug: str) -> str:
    text = source.read_text()

    fields = {name: value for name, value in EXTERN_RE.findall(text)}
    for required in ("Base", "End", "SlotCount"):
        if required not in fields:
            raise ValueError(f"{source}: no g_ps2RecompiledFunctionTable{required} definition")

    entries: list[tuple[int, str, str]] = []
    for line in text.splitlines():
        match = ENTRY_RE.match(line)
        if match:
            entries.append((int(match.group(1)), match.group(2), match.group(3) or ""))

    if not entries:
        raise ValueError(f"{source}: no table entries found")

    slot_count = int(fields["SlotCount"].rstrip("u"), 0)
    for slot, name, _ in entries:
        if slot >= slot_count:
            raise ValueError(f"{source}: slot {slot} ({name}) is outside the declared table")

    out = [
        HEADER.format(
            source=source,
            slug=slug,
            base=fields["Base"].rstrip("u"),
            end=fields["End"].rstrip("u"),
            slots=slot_count,
        )
    ]
    for slot, name, comment in entries:
        out.append(f"    g_slots[{slot}] = ::{name}; {comment}".rstrip() + "\n")
    out.append(FOOTER.format(slug=slug))
    return "".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("path", type=Path,
                        help="an overlay output directory, or with --all their parent")
    parser.add_argument("--slug", help="overlay slug (default: the directory name)")
    parser.add_argument("-o", "--output", type=Path,
                        help="output file (default: <dir>/overlay_table_<slug>.cpp)")
    parser.add_argument("--all", action="store_true",
                        help="treat `path` as the parent of every overlay output directory")
    args = parser.parse_args(argv)

    if args.all and (args.slug or args.output):
        parser.error("--all takes neither --slug nor --output")

    directories = (
        sorted(d for d in args.path.iterdir() if (d / "register_functions.cpp").is_file())
        if args.all
        else [args.path]
    )
    if not directories:
        print(f"error: no overlay output directories under {args.path}", file=sys.stderr)
        return 1

    status = 0
    for directory in directories:
        slug = args.slug or directory.name
        source = directory / "register_functions.cpp"
        target = args.output or (directory / f"overlay_table_{slug}.cpp")
        try:
            target.write_text(convert(source, slug))
        except (OSError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            status = 1
            continue
        print(f"{source} -> {target}")
    return status


if __name__ == "__main__":
    sys.exit(main())
