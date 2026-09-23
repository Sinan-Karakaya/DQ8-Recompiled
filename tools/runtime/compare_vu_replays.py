#!/usr/bin/env python3
"""Compare VU state, cycles and GIF output before reporting paired timings."""

import argparse
import glob
import re
import statistics
import subprocess
from pathlib import Path


def replay(binary, capture, repetitions):
    result = subprocess.run(
        [str(binary.resolve()), str(capture), str(repetitions)],
        check=True, capture_output=True, text=True,
    )
    match = re.search(r"^(VU[01] .*) us/run=([0-9.]+) n=\d+$", result.stdout, re.M)
    if not match:
        raise RuntimeError(f"Missing replay result: {result.stdout}\n{result.stderr}")
    return match[1], float(match[2])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("captures", help="Capture directory or .state glob")
    parser.add_argument("--repetitions", type=int, default=1000)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    if args.repetitions <= 0 or args.rounds <= 0:
        parser.error("repetitions and rounds must be positive")
    pattern = str(Path(args.captures) / "*.state") if Path(args.captures).is_dir() else args.captures
    captures = [Path(p).with_suffix("") for p in sorted(glob.glob(pattern))]
    if not captures:
        parser.error("no capture states found")
    binaries = [args.baseline, args.candidate]
    for capture in captures:
        timings = [[], []]
        expected = None
        for round_index in range(args.rounds):
            for index in ([0, 1] if round_index % 2 == 0 else [1, 0]):
                signature, elapsed = replay(binaries[index], capture, args.repetitions)
                if expected is None:
                    expected = signature
                if signature != expected:
                    raise RuntimeError(f"{capture.name}: differential mismatch\n{expected}\n{signature}")
                timings[index].append(elapsed)
        before, after = (statistics.median(times) for times in timings)
        print(f"{capture.name}: {expected} | median us {before:.3f} -> {after:.3f} "
              f"({(1 - after / before) * 100:+.1f}% time saved)", flush=True)
    print(f"PASS: {len(captures)} captures, {args.rounds} paired rounds; all signatures match")


if __name__ == "__main__":
    main()
