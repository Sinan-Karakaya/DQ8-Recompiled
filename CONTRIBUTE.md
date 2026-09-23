# Contributing

DQ8Recomp is an experimental static recompilation project. Start with the
[architecture](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Architecture)
and [current priorities](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Project-Status).
For a substantial change, open an issue describing the problem and proposed scope.

## Development workflow

1. Fork the repository and clone it with `--recurse-submodules`.
2. Create a branch for one change. Follow the
   [build guide](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Building).
3. Run the checks relevant to the change, following the
   [testing guide](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing).
4. Open a pull request describing the behavior changed, why, how it was tested,
   and any remaining limitations.

Generic PS2 behavior belongs in the PS2Recomp submodule. DQ8-specific integration
belongs here. A submodule update must refer to a commit accessible in its
configured remote; include the corresponding source changes in the review.

CI runs automatically on pushes to `main` and pull requests, including drafts.
It checks source distribution and tooling. In the public repository it also
builds the recompiler, runtime, and SDL GPU renderer on Linux x86-64, Linux ARM64,
and macOS ARM64, and runs authored runtime, VU differential, and graphics tests.
GPU tests report skips when a device is unavailable. While the repository is
private, only the lightweight Linux launcher and CPU graphics checks run alongside
the tooling checks, to limit paid runner usage. Full game builds, captured replays,
and live gameplay still require local game data.

## Code and tests

Use C++20 for runtime code and Python 3.10 or newer for scripts. Follow the style
of the surrounding code. Keep changes focused, avoid speculative abstractions,
and use comments to explain constraints or non-obvious behavior.

Preserve guest instruction semantics, event ordering, and simulation timing.
Renderer optimizations must preserve transfers, blending, depth, and synchronization.
VU changes need comparisons against the raw interpreter, including interrupted
execution and modified microcode. Prefer small, authored fixtures that expose the
bug over captured game data in a test commit.

For performance changes, compare identical workloads with competing work stopped.
Report actual completed gameplay frames as well as isolated timings. A faster
replay, title screen, or presentation counter alone does not prove faster gameplay.

## Game data and third-party code

Only contribute material you have the right to distribute under the applicable
license. Preserve copyright and license notices, and identify the source and
license of any imported code. Original contributions use the project's GPLv3
license unless an existing file specifies otherwise.

Do not commit disc images, BIOS files, game executables, generated game C++,
decryption keys, extracted assets, save files, or game-derived memory/graphics
captures. Keep these outside the checkout or in ignored build directories.
Do not attach them to issues, pull requests, CI artifacts, or releases.
Use your own lawfully obtained dump for local verification.

## Reporting problems

Include the commit, game version, OS, CPU/GPU, renderer, reproduction steps,
expected behavior, and observed behavior. Trim logs to the relevant diagnostics
and remove private paths or data. State whether a problem occurs with a clean
configuration. For visual regressions, identify the scene and renderer settings.

Issue forms add `bug` or `enhancement` and label the selected area; blank issues
receive `needs-triage`. PRs receive area labels from changed paths. Branch names
starting with `fix/`, `bugfix/`, or `hotfix/` add `bug`; `feat/` or `feature/`
add `enhancement` (a hyphen also works). Maintainers can adjust labels afterward.

User and developer documentation belongs in the
[wiki](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki). Document current
behavior and reproducible procedures; keep personal investigation logs and
handoffs local.
