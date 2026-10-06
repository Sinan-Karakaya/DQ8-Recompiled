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
launcher stub, not the game.

- [Running and controls](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Running)
- [Architecture](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Architecture)
- [Tests and debugging](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing)
- [Status and priorities](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Project-Status)

## In-game settings

With the SDL GPU renderer (`--gs=sdlgpu`), **F1** (or a controller's Guide
button, or Back+Start) opens a settings menu. While it is open the game gets no
input.

| Setting | Choices |
| --- | --- |
| Internal resolution | 1x (512x448, original) to 8x, applied immediately |
| Aspect ratio | Auto (default: follows the game's Screen Size), 4:3, 16:9, square pixels (8:7), fill window |
| Upscaling filter | Sharp bilinear (default), bilinear, nearest (integer scale) |
| Window | Fullscreen (also F11), frame-rate counter |

Widescreen is built into DQ8: set *Screen Size* to *Wide Screen 16:9* in the
game's own settings, and the Auto aspect shows it at 16:9.

Settings are saved to `settings.ini` in SDL's per-user preferences folder
(`~/.local/share/DQ8Recomp/DQ8Recomp/` on Linux); `--scale=N` still overrides
the internal resolution for one run.

## Contributing

See [CONTRIBUTE.md](CONTRIBUTE.md) for the workflow, testing expectations, and rules
for handling game data. Performance, compatibility, tooling, and documentation
contributions are welcome.

| Directory | Contents |
| --- | --- |
| `src/runtime` | Launcher, overlay dispatch, and game-specific integration |
| `src/gfx` | GS state, memory, trace replay, and SDL GPU renderer |
| `config` | Version-specific function maps and recompiler settings |
| `tools` | Extraction, analysis, code generation, and tests |
| `thirdparty` | Pinned PS2Recomp and SIMDe submodules |

Documentation is maintained in the
[wiki](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki).

## License and attribution

Project code is available under [GNU GPL version 3](LICENSE). Dependencies retain
their own licenses; see [NOTICE](NOTICE).

This is an independent project, unaffiliated with Square Enix, Level-5, or Sony.
Dragon Quest and PlayStation names belong to their respective owners. The project
license grants no rights to the game or its assets.
