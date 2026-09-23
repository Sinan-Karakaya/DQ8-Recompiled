#!/usr/bin/env bash
# DQ8Recomp — Ghidra function-map pipeline (main ELF + MWo3 overlays).
# Produces PS2Recomp-compatible function boundary CSVs in $OUT_DIR.
# Usage: GHIDRA_HOME=<installation> ELF=<disc ELF> ./run_analysis.sh [import|analyze|post|all]
set -euo pipefail

stage="${1:-all}"
case "$stage" in
    import|analyze|post|all) ;;
    *) echo "usage: $0 [import|analyze|post|all]" >&2; exit 2 ;;
esac
if [[ -z "${GHIDRA_HOME:-}" || -z "${ELF:-}" ]]; then
    echo "error: set GHIDRA_HOME to your Ghidra installation and ELF to your disc ELF" >&2
    exit 2
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROJECT_DIR="${PROJECT_DIR:-$REPO_ROOT/build/ghidra}"
PROJECT_NAME="${PROJECT_NAME:-DQ8}"
BIN_DIR="${BIN_DIR:-$(dirname "$ELF")/BIN}"
PROG_NAME="$(basename "$ELF")"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/config/$PROG_NAME}"
# Overlay BIN base names (space separated). PAL ships 5; NTSC-U adds VIEWER.
DEFAULT_OVERLAYS="TITLE BATTLE MENU SHOP CASINO"
if [[ "$PROG_NAME" == SLUS_212.07 ]]; then DEFAULT_OVERLAYS+=" VIEWER"; fi
read -r -a OVERLAY_NAMES <<< "${OVERLAYS:-$DEFAULT_OVERLAYS}"
LOG_DIR="${LOG_DIR:-$PROJECT_DIR/logs}"
LANG_ID="r5900:LE:32:default"          # ghidra-emotionengine-reloaded EE language
ANALYSIS_TIMEOUT="${ANALYSIS_TIMEOUT:-5400}"   # seconds per file
export MAXMEM="${MAXMEM:-6G}"
# Ghidra needs a full JDK; use JAVA_HOME when supplied, otherwise the host PATH.
if [[ -n "${JAVA_HOME:-}" ]]; then export PATH="$JAVA_HOME/bin:$PATH"; fi
if ! command -v javac >/dev/null 2>&1; then
    echo "error: a full JDK is required; install one or set JAVA_HOME" >&2
    exit 2
fi

HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
if [[ ! -x "$HEADLESS" ]]; then
    echo "error: Ghidra headless launcher not executable: $HEADLESS" >&2
    exit 2
fi
if [[ "$stage" == import || "$stage" == all ]]; then
    if [[ ! -f "$ELF" || ! -d "$BIN_DIR" ]]; then
        echo "error: import requires ELF file $ELF and overlay directory $BIN_DIR" >&2
        exit 2
    fi
fi
mkdir -p "$PROJECT_DIR" "$LOG_DIR" "$OUT_DIR"

# Computes the main-image layout from the ELF program headers. Prints 5 hex
# values: TEXT_END BSS_START BSS_END OVL_START OVL_END where
#   TEXT_END  = (address of last `jr ra` word in the first PT_LOAD) + 8,
#               rounded up to 16 (everything after is treated as data),
#   BSS       = memsz-beyond-filesz of the first PT_LOAD,
#   OVL       = union of the zero-filesize PT_LOADs (overlay staging region).
find_layout() {
python3 - "$ELF" <<'EOF'
import struct, sys
f = open(sys.argv[1], 'rb').read()
e_phoff, = struct.unpack_from('<I', f, 0x1C)
e_phnum, = struct.unpack_from('<H', f, 0x2C)
text_end = bss = (0, 0)
ovl_lo, ovl_hi = [], []
first = True
for i in range(e_phnum):
    p_type, p_off, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from('<6I', f, e_phoff + i*32)
    if p_type != 1:
        continue
    if p_filesz > 0 and first:
        first = False
        seg = f[p_off:p_off+p_filesz]
        words = struct.unpack('<%dI' % (len(seg)//4), seg[:len(seg)//4*4])
        last = max(j for j, w in enumerate(words) if w == 0x03e00008)
        text_end = (p_vaddr + last*4 + 8 + 15) & ~15   # jr ra + delay slot
        bss = (p_vaddr + p_filesz, p_vaddr + p_memsz)
    elif p_filesz == 0 and p_memsz > 0:
        ovl_lo.append(p_vaddr)
        ovl_hi.append(p_vaddr + p_memsz)
ovl = (min(ovl_lo), max(ovl_hi)) if ovl_lo else (0, 0)
print('0x%08x 0x%08x 0x%08x 0x%08x 0x%08x' % (text_end, bss[0], bss[1], ovl[0], ovl[1]))
EOF
}

if [[ "$stage" == "import" || "$stage" == "all" ]]; then
    read -r TEXT_END BSS_START BSS_END OVL_START OVL_END <<< "$(find_layout)"
    echo "== import: $ELF (lang $LANG_ID), .text end = $TEXT_END, bss = [$BSS_START,$BSS_END), overlay stage = [$OVL_START,$OVL_END)"
    "$HEADLESS" "$PROJECT_DIR" "$PROJECT_NAME" \
        -import "$ELF" -overwrite \
        -processor "$LANG_ID" -cspec default \
        -noanalysis \
        -scriptPath "$SCRIPT_DIR" \
        -postScript ImportDQ8Main.java "$TEXT_END" "$BSS_START" "$BSS_END" "$OVL_START" "$OVL_END" \
        -postScript ImportDQ8Overlays.java "$BIN_DIR" "${OVERLAY_NAMES[@]}" \
        -log "$LOG_DIR/import.log"
fi

if [[ "$stage" == "analyze" || "$stage" == "all" ]]; then
    echo "== analyze: full auto-analysis (this takes a while)"
    "$HEADLESS" "$PROJECT_DIR" "$PROJECT_NAME" \
        -process "$PROG_NAME" \
        -analysisTimeoutPerFile "$ANALYSIS_TIMEOUT" \
        -scriptPath "$SCRIPT_DIR" \
        -log "$LOG_DIR/analysis.log"
fi

if [[ "$stage" == "post" || "$stage" == "all" ]]; then
    echo "== post: sweep + names + CSV export to $OUT_DIR"
    "$HEADLESS" "$PROJECT_DIR" "$PROJECT_NAME" \
        -process "$PROG_NAME" \
        -noanalysis \
        -scriptPath "$SCRIPT_DIR" \
        -postScript PostAnalysisSweep.java \
        -postScript PruneJunkFunctions.java \
        -postScript ApplyKnownNames.java \
        -postScript ExportDQ8Csvs.java "$OUT_DIR" "${OVERLAY_NAMES[@]}" \
        -log "$LOG_DIR/post.log"
fi

echo "done."
