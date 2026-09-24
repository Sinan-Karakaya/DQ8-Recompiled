#!/usr/bin/env python3
"""Turn ps2_analyzer's stub proposals into a vetted [general].stubs list.

Only names the runtime actually implements as stubs are emitted. Anything else
is reported so it can be handled deliberately rather than breaking the build.

DO_NOT_STUB records the reverse case: names the runtime does implement that we
deliberately keep as guest code, so a regeneration cannot reintroduce them.
"""
import re
import sys

ANALYZER_TOML = sys.argv[1]
CALL_LIST = sys.argv[2]

# The C allocator has to have exactly one owner, and DQ8's is the guest's.
# See https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Architecture.
DO_NOT_STUB = {
    "malloc": "guest owns the C heap",
    "memalign": "same arena as guest malloc",
    "free": "same arena as guest malloc",
    "malloc_extend_top": "newlib arena growth; the runtime's version always fails",
    "__malloc_lock": "guest semaphore, created by supplement_crt0",
    "__malloc_unlock": "pairs with __malloc_lock",
    # SDRDRV runs natively, and the Sound Kit rides on these too.
    "sceSdRemoteInit": "libsdr binds the native SDRDRV over SIF",
    "sceSdRemote": "libsdr packs its own RPCs; SK commands depend on it",
    "sceSdTransToIOP": "plain SIF DMA into IOP memory",
}


def macro_names(path, macro):
    """Names inside one X-macro list, following backslash continuations."""
    out, active = set(), False
    for line in open(path):
        if f"#define {macro}" in line:
            active = True
        if active:
            out.update(re.findall(r"X\(([A-Za-z_][A-Za-z0-9_]*)\)", line))
            if not line.rstrip().endswith("\\"):
                break
    return out


stubs = macro_names(CALL_LIST, "PS2_STUB_LIST")
syscalls = macro_names(CALL_LIST, "PS2_SYSCALL_LIST")

proposed = []
in_list = False
for line in open(ANALYZER_TOML):
    if line.startswith("stubs = ["):
        in_list = True
        continue
    if in_list:
        if line.startswith("]"):
            break
        m = re.search(r'"([^"@]+)@(0x[0-9A-Fa-f]+)"', line)
        if m:
            proposed.append((m.group(1), m.group(2)))

accepted, as_syscall, unknown, kept_guest = [], [], [], []
for name, addr in proposed:
    if name in DO_NOT_STUB:
        kept_guest.append((name, addr))
    elif name in stubs:
        accepted.append((name, addr))
    elif name in syscalls:
        as_syscall.append((name, addr))
    else:
        unknown.append((name, addr))

sys.stderr.write(
    f"proposed={len(proposed)} accepted={len(accepted)} "
    f"kept-guest={len(kept_guest)} "
    f"syscall-list={len(as_syscall)} unknown={len(unknown)}\n"
)
for name, addr in kept_guest:
    sys.stderr.write(f"  kept guest  : {name}@{addr}  ({DO_NOT_STUB[name]})\n")
for name, addr in as_syscall:
    sys.stderr.write(f"  syscall-list: {name}@{addr}\n")
for name, addr in unknown:
    alt = name.lstrip("_")
    hint = f"  (runtime spells it '{alt}')" if alt in stubs else ""
    sys.stderr.write(f"  unknown     : {name}@{addr}{hint}\n")

print("stubs = [")
for name, addr in accepted:
    print(f'    "{name}@{addr}",')
print("]")
