# DQ8Recomp

An experimental native port of **Dragon Quest VIII: Journey of the Cursed King**
for PlayStation 2, built with [PS2Recomp](https://github.com/ran-j/PS2Recomp).
The game's MIPS code is translated to C++ locally and linked to a runtime that
implements the PS2 services it uses. Graphics use SDL3 GPU or a software reference
renderer.

**This is a development project, not a finished port.** The NTSC-U version
(`SLUS_212.07`) boots, loads saves, and reaches field gameplay, with sound from
the game's own drivers. Sustained playable performance and full-game
compatibility are still in progress. The PAL configuration is incomplete.

## Getting started

You need your own lawfully obtained game dump. This repository provides tools,
runtime code, and configuration; it does not provide the game, a BIOS, or a
prebuilt game executable.

```sh
git clone --recurse-submodules https://github.com/Sinan-Karakaya/DQ8-Recompiled.git
cd DQ8-Recompiled
```

Follow the [build guide](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Building)
to generate the game code and build the runtime. A default CMake build produces a
stub executable, not the game.

- [Running and controls](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Running)
- [Architecture](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Architecture)
- [Tests and debugging](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing)
- [Status and priorities](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Project-Status)

### The launcher

Or let the launcher do it. It is a small window: drop your disc image on it, and
it checks the disc, copies its files, translates and compiles the game on your
computer, and starts it. Nothing from the game is downloaded or shared.

Its release download carries everything the build needs (the source, CMake, Ninja,
Python, SDL3, FFmpeg and the libraries the build would fetch) except a C++
compiler, which no release may include. If yours is missing, the launcher
installs it through your system: Apple's Command Line Tools on macOS, Visual
Studio Build Tools on Windows, your distribution's packages on Linux. The first
build takes about ten minutes on a recent computer, longer with fewer cores.

From a checkout instead, it uses the tools on your machine, and can install what
is missing:

```sh
cmake -S tools/launcher -B build/launcher -G Ninja
cmake --build build/launcher
```

Then open `build/launcher/DQ8Recomp Launcher.app` on macOS, or run
`build/launcher/dq8-launcher` on Linux and Windows. `dq8-launcher --build --disc
<image.iso>` runs the same build without the window.
`python3 tools/launcher/payload.py --out build/payload` assembles a release's
payload, as the Release workflow does for each platform.

## Contributing

See [CONTRIBUTE.md](CONTRIBUTE.md) for the workflow, testing expectations, and rules
for handling game data. Performance, compatibility, tooling, and documentation
contributions are welcome.

| Directory | Contents |
| --- | --- |
| `src/runtime` | Entry point, overlay dispatch, and game-specific integration |
| `src/gfx` | GS state, memory, trace replay, and SDL GPU renderer |
| `src/ui` | In-game menu (F1): display, sound, controls and quality-of-life settings |
| `config` | Version-specific function maps and recompiler settings |
| `tools` | The launcher, extraction, analysis, code generation, and tests |
| `thirdparty` | Pinned PS2Recomp, SIMDe and Dear ImGui submodules |

Documentation is maintained in the
[wiki](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki).

## License and attribution

Project code is available under [GNU GPL version 3](LICENSE). Dependencies retain
their own licenses; see [NOTICE](NOTICE).

This is an independent project, unaffiliated with Square Enix, Level-5, or Sony.
Dragon Quest and PlayStation names belong to their respective owners. The project
license grants no rights to the game or its assets.
