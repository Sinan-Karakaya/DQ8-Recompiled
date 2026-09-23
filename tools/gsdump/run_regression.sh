#!/usr/bin/env bash
# Replay every GS dump in a directory and diff each against PCSX2's screenshot.
#
# Needs no game dump and no recompiled code -- only .gs dumps the user captured.
# Thresholds are deliberately loose: the reference is PCSX2's post-processed
# output resampled onto our native grid, so edges never match exactly. See
# https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing.
#
#   run_regression.sh [dump-dir] [out-dir]

set -uo pipefail

REPLAY="${DQ8_GSREPLAY:-}"
if [ -z "$REPLAY" ]; then
    for candidate in build/gsdump/dq8-gsreplay ../build/gsdump/dq8-gsreplay; do
        [ -x "$candidate" ] && REPLAY="$candidate" && break
    done
fi
if [ ! -x "${REPLAY:-}" ]; then
    echo "error: dq8-gsreplay not found; build it or set DQ8_GSREPLAY" >&2
    exit 2
fi

DUMPS="${1:-../gsdumps}"
OUT="${2:-build/gsdump/regression}"
MIN_PSNR="${DQ8_GS_MIN_PSNR:-26}"

if [ ! -d "$DUMPS" ]; then
    echo "skip: no dump directory at $DUMPS" >&2
    exit 0
fi

shopt -s nullglob
files=("$DUMPS"/*.gs "$DUMPS"/*.gs.zst "$DUMPS"/*.gs.xz)
if [ ${#files[@]} -eq 0 ]; then
    echo "skip: no .gs dumps in $DUMPS" >&2
    exit 0
fi

mkdir -p "$OUT"
fail=0
for dump in "${files[@]}"; do
    name=$(basename "$dump" | sed 's/\.gs\(\.zst\|\.xz\)\?$//')
    if ! "$REPLAY" replay "$dump" --out "$OUT/$name" --require-content >"$OUT/$name.replay.log" 2>&1; then
        echo "FAIL $name: replay produced no content"
        fail=1
        continue
    fi
    if ! "$REPLAY" shot "$dump" --out "$OUT/$name.ref" >/dev/null 2>&1; then
        echo "SKIP $name: dump carries no screenshot"
        continue
    fi
    result=$("$REPLAY" diff "$OUT/$name.raw32" "$OUT/$name.ref.raw32" \
                 --fit b --ignore-alpha --min-psnr "$MIN_PSNR" \
                 --heatmap "$OUT/$name.heat.png" 2>&1)
    status=$?
    psnr=$(printf '%s\n' "$result" | awk '/^psnr/ {print $3}')
    if [ $status -ne 0 ]; then
        echo "FAIL $name: psnr ${psnr:-?} dB (min $MIN_PSNR)"
        fail=1
    else
        echo "ok   $name: psnr ${psnr:-?} dB"
    fi
done

exit $fail
