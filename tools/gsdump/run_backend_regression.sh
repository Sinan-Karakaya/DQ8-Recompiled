#!/usr/bin/env bash
# Replays every dump through both raster backends and diffs hardware against
# software. The software backend is the reference: it is the one whose output
# has been checked against PCSX2.
#
#   run_backend_regression.sh <dump-dir> <out-dir> [min-psnr]
#
# Skips cleanly when no dumps are present, so it is safe to wire into CI before
# anyone has captured any.

set -uo pipefail

DUMP_DIR="${1:-}"
OUT_DIR="${2:-}"
MIN_PSNR="${3:-25}"
REPLAY="${DQ8_GSREPLAY:-}"

if [[ -z "$DUMP_DIR" || -z "$OUT_DIR" ]]; then
    echo "usage: $0 <dump-dir> <out-dir> [min-psnr]" >&2
    exit 2
fi
if [[ -z "$REPLAY" ]]; then
    echo "error: set DQ8_GSREPLAY to the dq8-gsreplay binary" >&2
    exit 2
fi
if [[ ! -d "$DUMP_DIR" ]]; then
    echo "no dump directory at $DUMP_DIR -- skipping backend regression"
    exit 0
fi

shopt -s nullglob
DUMPS=("$DUMP_DIR"/*.gs "$DUMP_DIR"/*.gs.xz "$DUMP_DIR"/*.gs.zst)
if [[ ${#DUMPS[@]} -eq 0 ]]; then
    echo "no dumps under $DUMP_DIR -- skipping backend regression"
    exit 0
fi

mkdir -p "$OUT_DIR"
# The backend needs a GPU device, not a window.
if [[ "$(uname -s)" != Darwin ]]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"
fi

FAILURES=0
printf '%-20s %10s %10s %12s\n' dump psnr mean-err verdict
for DUMP in "${DUMPS[@]}"; do
    NAME="$(basename "$DUMP")"
    NAME="${NAME%.xz}"
    NAME="${NAME%.zst}"
    NAME="${NAME%.gs}"
    SW="$OUT_DIR/${NAME}_sw"
    HW="$OUT_DIR/${NAME}_hw"

    if ! "$REPLAY" replay "$DUMP" --out "$SW" --last --gs sw >"$OUT_DIR/${NAME}_sw.log" 2>&1; then
        printf '%-20s %10s %10s %12s\n' "$NAME" - - "sw-failed"
        FAILURES=$((FAILURES + 1))
        continue
    fi
    if ! "$REPLAY" replay "$DUMP" --out "$HW" --last --gs sdlgpu >"$OUT_DIR/${NAME}_hw.log" 2>&1; then
        # No GPU device is a skip, not a failure: CI runners often have none.
        if grep -q "SDL GPU backend unavailable" "$OUT_DIR/${NAME}_hw.log"; then
            printf '%-20s %10s %10s %12s\n' "$NAME" - - "no-device"
            continue
        fi
        printf '%-20s %10s %10s %12s\n' "$NAME" - - "hw-failed"
        FAILURES=$((FAILURES + 1))
        continue
    fi

    DIFF="$("$REPLAY" diff "$SW.raw32" "$HW.raw32" --tolerance 8 --ignore-alpha \
            --heatmap "$OUT_DIR/${NAME}_heat.png" 2>/dev/null)"
    PSNR="$(sed -n 's/^psnr *: *\([0-9.]*\).*/\1/p' <<<"$DIFF")"
    MEAN="$(sed -n 's/^mean abs error *: *\([0-9.]*\).*/\1/p' <<<"$DIFF")"
    if [[ -z "$PSNR" ]]; then
        printf '%-20s %10s %10s %12s\n' "$NAME" - - "diff-failed"
        FAILURES=$((FAILURES + 1))
        continue
    fi

    if awk "BEGIN { exit !($PSNR >= $MIN_PSNR) }"; then
        printf '%-20s %10s %10s %12s\n' "$NAME" "$PSNR" "$MEAN" "ok"
    else
        printf '%-20s %10s %10s %12s\n' "$NAME" "$PSNR" "$MEAN" "below-$MIN_PSNR"
        FAILURES=$((FAILURES + 1))
    fi
done

if [[ $FAILURES -ne 0 ]]; then
    echo "$FAILURES dump(s) failed the hardware/software comparison" >&2
    exit 1
fi
echo "all dumps within tolerance (min psnr $MIN_PSNR)"
