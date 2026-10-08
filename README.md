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

## Play it

You need:

- a computer running Windows, macOS or Linux, with about 8 GB of free space;
- a disc image (an `.iso` file) made from your own copy of the North American
  release of the game (SLUS-21207).

Nothing from the game is downloaded: the launcher builds the game on your
computer, from your disc.

1. Open the [latest release](https://github.com/Sinan-Karakaya/DQ8-Recompiled/releases/latest),
   scroll down to **Assets**, and click the file for your computer to download it:

   | Your computer | File to download |
   | --- | --- |
   | Windows | `DQ8Recomp-…-Windows-x86_64.zip` |
   | Mac with Apple silicon (M1 or newer) | `DQ8Recomp-…-macOS-arm64.zip` |
   | Mac with an Intel processor | `DQ8Recomp-…-macOS-x86_64.zip` |
   | Linux on a PC | `DQ8Recomp-…-Linux-x86_64.tar.gz` |
   | Linux on ARM | `DQ8Recomp-…-Linux-arm64.tar.gz` |

   Not sure which Mac you have? In the Apple menu, choose **About This Mac**: "Apple
   M1" or later means Apple silicon, and "Intel" means Intel. The other files in
   the list are for developers.

2. Unpack it. On Windows, right-click the file and choose **Extract All**: the
   launcher can't run from inside the zip. On macOS, double-click it. On Linux,
   use your file manager's **Extract**.

3. Open the launcher.
   - **Windows:** open `dq8-launcher` in the `DQ8Recomp` folder. If Windows says it
     protected your PC, choose **More info**, then **Run anyway**.
   - **macOS:** the app isn't signed yet, so macOS blocks it. The first time, open
     Terminal, type `xattr -dr com.apple.quarantine ` (with the space at the end),
     drag **DQ8Recomp Launcher** onto the Terminal window, and press Return. That
     unblocks the app and the tools inside it. Then double-click the app.
   - **Linux:** open `dq8-launcher` in the `DQ8Recomp` folder.

4. Drag your disc image onto the launcher's window, and go through its pages with
   **Continue**. If something is missing, such as a C++ compiler, press **Install
   for me**; your computer may ask for your permission.

5. Press **Start building**. The first build takes about ten minutes on a recent
   computer, longer on an older one. Then press **Play**.

Your game files and saves are kept in a `DQ8Recomp` folder in your home folder;
the launcher's Options page can choose another before you build. To update,
download the latest release again and open it: your saves stay, and the build
only redoes what changed. In the game, **F1** opens the menu for controls,
display and sound. [Running and controls](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Running)
lists the keyboard and gamepad buttons.

## Build from source

This repository provides tools, runtime code, and configuration; it does not
provide the game, a BIOS, or a prebuilt game executable. You need your own
lawfully obtained game dump.

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

### The launcher from a checkout

The launcher checks the disc, copies its files, translates and compiles the game,
and starts it. From a checkout, it uses the tools on your machine, and can
install what is missing:

```sh
cmake -S tools/launcher -B build/launcher -G Ninja
cmake --build build/launcher
```

Then open `build/launcher/DQ8Recomp Launcher.app` on macOS, or run
`build/launcher/dq8-launcher` on Linux and Windows. `dq8-launcher --build --disc
<image.iso>` runs the same build without the window.

Each push to `main` is published to
[Releases](https://github.com/Sinan-Karakaya/DQ8-Recompiled/releases), named by
its commit's short hash. A release download carries everything the build needs
(the source, CMake, Ninja, Python, SDL3, FFmpeg and the libraries the build
would fetch) except a C++ compiler, which no release may include; the launcher
installs that through the system: Apple's Command Line Tools on macOS, Visual
Studio Build Tools on Windows, the distribution's packages on Linux.
`python3 tools/launcher/payload.py --out build/payload` assembles a release's
payload, as the Release workflow does for each platform: macOS on Apple silicon
and Intel, Windows on x86-64, Linux on x86-64 and ARM64.

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
