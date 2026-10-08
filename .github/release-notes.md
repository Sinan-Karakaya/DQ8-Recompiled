Built from @COMMIT@ on `main`.

These downloads build Dragon Quest VIII from your own disc: the launcher translates and compiles the game on your computer, and nothing from the game is in them. You need a disc image of the North American release (SLUS-21207).

| Your computer | Download |
| --- | --- |
| Mac with Apple silicon | `DQ8Recomp-@VERSION@-macOS-arm64.zip` |
| Mac with an Intel processor | `DQ8Recomp-@VERSION@-macOS-x86_64.zip` |
| Windows | `DQ8Recomp-@VERSION@-Windows-x86_64.zip` |
| Linux on x86-64 | `DQ8Recomp-@VERSION@-Linux-x86_64.tar.gz` |
| Linux on ARM64 | `DQ8Recomp-@VERSION@-Linux-arm64.tar.gz` |

1. Unpack it and open **DQ8Recomp Launcher**, or `dq8-launcher` on Windows and Linux.
2. Drop your disc image on its window and go through its pages with **Continue**. If something is missing, such as a C++ compiler, **Install for me** installs it.
3. Press **Start building**. The first build takes about ten minutes on a recent computer, longer with fewer cores; later ones only redo what changed.
4. Press Play.

**macOS** won't open the app the first time, because it isn't signed yet. Open System Settings, then Privacy & Security, and choose **Open Anyway**. Or run this in Terminal: `xattr -dr com.apple.quarantine "DQ8Recomp Launcher.app"`.

**Windows** may warn about an unrecognized app: choose **More info**, then **Run anyway**. The build downloads FFmpeg once, so it needs an internet connection.

Also attached:

- `DQ8Recomp-@VERSION@-source.tar.gz`: the source with its submodules, which GitHub's own source archives leave out.
- `@FFMPEG_SOURCE@`: the source of the FFmpeg libraries in the macOS and Linux downloads, under the LGPL 2.1.
- `SHA256SUMS.txt`: the SHA-256 of every file.

Each download lists the other projects it carries, with their licenses, in `payload/licenses`. On macOS that folder is inside the app.
