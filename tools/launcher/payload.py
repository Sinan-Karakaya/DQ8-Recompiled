#!/usr/bin/env python3
"""Assembles the launcher's payload: everything the game build needs besides a
C++ compiler, so a player downloads one archive and installs nothing else.

    payload/
      source/        this checkout with its submodules, tracked files only
      fetch/         the sources CMake's FetchContent would clone with git
      tools/         Python, CMake, Ninja and pkgconf, relocatable
      deps/          SDL3 (static) and FFmpeg (shared, MPEG-2 only)
      licenses/      the licenses of what tools/ and deps/ were built from
      payload.json   versions and where each tool lives

Run it from a checkout with its submodules, on the platform it is for, with a
compiler but no system glslang or spirv-cross (they would hide the sources the
build fetches without them):

    python3 tools/launcher/payload.py --out build/payload

Every download is pinned by SHA-256. Windows takes FFmpeg from PS2Recomp's own
prebuilt download at build time, so its payload has no FFmpeg.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

PYTHON_RELEASE = "20261003"
PYTHON_VERSION = "3.12.15"
# python-build-standalone, install_only_stripped.
PYTHON = {
    "macos-arm64": ("aarch64-apple-darwin", "ad8d0c637c0a36b967b310e2c07254f4d2ca8cabaa7699e55ed6290aceb481a2"),
    "macos-x86_64": ("x86_64-apple-darwin", "562c30864ece2cb1d3e0ad66a1acd498611a47e5a10ce81b99158bef1ccbd355"),
    "windows-x86_64": ("x86_64-pc-windows-msvc", "6fba7f2ae506facf41d457ea8293c7497910a675c69a4e954875169410a50402"),
    "linux-x86_64": ("x86_64-unknown-linux-gnu", "731af898886c5f821890dc901eca3c651cca8e51fa7308c159d12a1194aeac91"),
    "linux-arm64": ("aarch64-unknown-linux-gnu", "6541297dd1798dec8b98c3ad7492808a5b9d1c126801ceb2011e7754cd20d1ce"),
}

PYPI = "https://files.pythonhosted.org/packages/"
# PyPI wheels: (path under PYPI, SHA-256). Only their binaries are used.
WHEELS = {
    "cmake": ("4.4.4", "cmake/data/", {
        "macos": ("3f/02/86b484e16c91de4bdf90dfab37a33c3e5512bc30d3a494759976df0943b9/"
                  "cmake-4.4.4-py3-none-macosx_10_10_universal2.whl",
                  "883962a72b1b3a16ff445427f24c8caca9797dd9e563bbe208cbcd89e877f9d3"),
        "windows-x86_64": ("87/9c/a7918eeeed09b34aa5dc1b767367fb1a2dcbb56171be00be0fd1280fe752/"
                           "cmake-4.4.4-py3-none-win_amd64.whl",
                           "ad8e0a38b5707e27882701146bdfffecacd80e7703fa0fc36c77528518d545af"),
        "linux-x86_64": ("7e/d5/664ea06d01864f25d6067f09fa218404e4d588edd5e495135ac91429fecb/"
                         "cmake-4.4.4-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.whl",
                         "f773a0544c66370408f451acd487c2c643b0dfebb48c5d64f3638bc1bb820238"),
        "linux-arm64": ("76/0a/f6c1119b24ca4b81476f9398e90f3f8175756dfe16e082c691b12992c991/"
                        "cmake-4.4.4-py3-none-manylinux2014_aarch64.manylinux_2_17_aarch64.whl",
                        "b3029f586853e01ddf2824c1ff124a1c5284f8ae897994758e9c4eeb45903b58"),
    }),
    "ninja": ("1.13.2", "ninja-1.13.2.data/scripts/", {
        "macos": ("b5/b8/90a9518f2264637084d6199bc20c2f3fb97fefc4c474d277720150c3bd6d/"
                  "ninja-1.13.2-py3-none-macosx_10_9_universal2.whl",
                  "fd82e26c0706ad4ab88e5fdd26f3fab0a987a90f810160f6c322e752c6af298b"),
        "windows-x86_64": ("3f/dd/3766b5f4d32e8a9b97d195496b0b01fbbe2e1a41669dab0cd6492a6ce199/"
                           "ninja-1.13.2-py3-none-win_amd64.whl",
                           "1293f4078278b70d0ee4b6cc8f3a9e030656c9b2f59909970343c4fe76070118"),
        "linux-x86_64": ("6e/53/ebfed7b689c338dd8ebeec9c0730c8d56821292f14e2536e5f3ef1a05744/"
                         "ninja-1.13.2-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.whl",
                         "65a24341b5ac09fcadcc37082660be40a94174e51a937fabf6e2cae26225fa2c"),
        "linux-arm64": ("35/54/7368ce188625e39acc03ee362bb86cc9bfa6ad15e25c50889ae36e2889b3/"
                        "ninja-1.13.2-py3-none-manylinux2014_aarch64.manylinux_2_17_aarch64.whl",
                        "d775a5e43e9088f507a6250d57fcf5678eb31268c545feb5064ffeee33735622"),
    }),
    "pkgconf": ("3.0.7", "pkgconf/.bin/", {
        "macos-arm64": ("bf/67/fcd20a9e78fd6bba952e982a86e98e0240a95a216143eab389cbef045d43/"
                        "pkgconf-3.0.7.post0-py3-none-macosx_11_0_arm64.whl",
                        "47cbf6889d84297b9a4e1741ae016daa4e198f3ec8bab7ce74554cc50b77cce4"),
        "macos-x86_64": ("df/ea/3d7f83f95f53190d1b9abb9f67b5dfdc3ece092110a0bc1fc60115dcfad9/"
                         "pkgconf-3.0.7.post0-py3-none-macosx_10_9_x86_64.whl",
                         "3a688f02e12602b4a2be79d9a54ed598d5c44fc2fe042a7501aae53054e4688d"),
        "windows-x86_64": ("e2/53/37ee781984b17e16c9b09726c438a327b242721166213411623814b89ddf/"
                           "pkgconf-3.0.7.post0-py3-none-win_amd64.whl",
                           "e7906bb5ec1d0d074e397466075a8545f1c50cb11710768644f222ba8acebc7f"),
        "linux-x86_64": ("6c/eb/4a4fe6c178d4447f3a3189b5fb740a76dc7ef8846a57886270315f22f7ee/"
                         "pkgconf-3.0.7.post0-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64."
                         "manylinux_2_28_x86_64.whl",
                         "bd3ee4b5294a888d58462324cb83b7c0bc7150cc15b64b36aaf983e4b0ab90d5"),
        "linux-arm64": ("42/b9/6c5293a5c65de1152db827cffaaede42a001c5913927e1ef2e8e8aae139f/"
                        "pkgconf-3.0.7.post0-py3-none-manylinux2014_aarch64.manylinux_2_17_aarch64."
                        "manylinux_2_28_aarch64.whl",
                        "4ece8d4e650ee172ddadff762ad31a590a4d91a070806aee09eb92a89e67f559"),
    }),
}

SDL3 = ("3.4.16", "https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz",
        "7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68")
# Pinned from ffmpeg.org over HTTPS; the release's PGP signature is not checked here.
FFMPEG = ("7.1.5", "https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz",
          "de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f")

# The runtime uses FFmpeg's MPEG-2 decoder and parser and swscale, nothing else.
FFMPEG_CONFIGURE = [
    "--disable-everything", "--disable-autodetect", "--disable-programs", "--disable-doc",
    "--disable-network", "--disable-x86asm", "--enable-shared", "--disable-static", "--enable-pic",
    "--enable-decoder=mpeg2video", "--enable-parser=mpegvideo",
    "--enable-avcodec", "--enable-avformat", "--enable-avutil", "--enable-swscale", "--enable-swresample",
]
MACOS_DEPLOYMENT_TARGET = "11.0"

# Never used to build the game, and half the payload: CMake's other programs
# and manuals, Python's pip, GUI and installer, raylib's examples (built only
# with BUILD_EXAMPLES, off for a subproject).
PRUNE = [
    "tools/cmake/bin/ccmake", "tools/cmake/bin/cpack", "tools/cmake/bin/ctest",
    "tools/cmake/bin/cmake-gui", "tools/cmake/doc", "tools/cmake/man",
    "tools/python/lib/python3.12/site-packages/pip", "tools/python/lib/python3.12/idlelib",
    "tools/python/lib/python3.12/ensurepip", "tools/python/lib/python3.12/tkinter",
    "tools/python/lib/python3.12/turtledemo", "tools/python/Lib/site-packages/pip",
    "tools/python/Lib/idlelib", "tools/python/Lib/ensurepip", "tools/python/Lib/tkinter",
    "tools/python/tcl", "fetch/raylib-src/examples", "fetch/raylib-src/projects",
]


def prune(out: Path, target: str) -> None:
    for name in PRUNE:
        for path in (out / name, out / (name + ".exe")):
            if path.is_dir():
                shutil.rmtree(path)
            elif path.exists():
                path.unlink()
    # macOS and Linux keep Tcl and Tk in lib/, beside python3.12/. Windows keeps
    # them in tcl/, pruned above, and its Lib/ is the standard library itself,
    # which lib/ finds on a case-blind disk: thread* would take threading.py.
    if not target.startswith("windows"):
        lib = out / "tools" / "python" / "lib"
        for pattern in ("tcl*", "tk*", "libtcl*", "libtk*", "itcl*", "thread*"):
            for path in lib.glob(pattern):
                shutil.rmtree(path) if path.is_dir() else path.unlink()
    # CMake's manual, wherever the wheel puts its docs.
    for path in (out / "tools" / "cmake").rglob("*.qch"):
        path.unlink()


def host_platform() -> str:
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x86_64"
    if sys.platform == "darwin":
        return f"macos-{arch}"
    if sys.platform == "win32":
        return f"windows-{arch}"
    return f"linux-{arch}"


def exe(name: str, target: str) -> str:
    return name + ".exe" if target.startswith("windows") else name


def fetch(url: str, sha256: str, cache: Path) -> Path:
    """Downloads url into cache once; refuses anything but the pinned bytes."""
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / url.rsplit("/", 1)[1].replace("%2B", "+")
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != sha256:
        print(f"payload: downloading {url}", flush=True)
        partial = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url) as response, open(partial, "wb") as out:
            shutil.copyfileobj(response, out)
        digest = hashlib.sha256(partial.read_bytes()).hexdigest()
        if digest != sha256:
            partial.unlink()
            sys.exit(f"payload: {url} has SHA-256 {digest}, expected {sha256}")
        partial.replace(path)
    return path


def untar(archive: Path, dest: Path) -> None:
    with tarfile.open(archive) as tar:
        try:
            tar.extractall(dest, filter="tar")
        except TypeError:  # Python before 3.12 and its backports
            tar.extractall(dest)


def unpack_wheel(wheel: Path, prefix: str, dest: Path) -> None:
    """Copies the wheel members under prefix into dest, keeping exec bits."""
    with zipfile.ZipFile(wheel) as archive:
        for info in archive.infolist():
            if not info.filename.startswith(prefix) or info.is_dir():
                continue
            target = dest / info.filename[len(prefix):]
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, open(target, "wb") as out:
                shutil.copyfileobj(source, out)
            mode = (info.external_attr >> 16) & 0o777
            if mode:
                os.chmod(target, mode)


def run(args: list, cwd: Path | None = None, env: dict | None = None) -> None:
    print("payload: $ " + " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)


def checkout_version(repo: Path) -> str:
    """The commit's short hash, which names releases, and -dirty for local edits."""
    def git(*args: str) -> str:
        return subprocess.run(["git", "-C", repo, *args], check=True, capture_output=True, text=True).stdout.strip()
    dirty = git("status", "--porcelain", "--untracked-files=no")
    return git("rev-parse", "--short=7", "HEAD") + ("-dirty" if dirty else "")


def copy_source(repo: Path, dest: Path, version: str) -> None:
    """The checkout's tracked files, submodules included, and its version."""
    listing = subprocess.run(["git", "-C", repo, "ls-files", "--recurse-submodules", "-z"],
                             check=True, capture_output=True).stdout.decode()
    for name in filter(None, listing.split("\0")):
        source = repo / name
        if source.is_dir() and not source.is_symlink():
            continue  # a submodule git did not recurse into
        target = dest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            target.symlink_to(os.readlink(source))
        else:
            shutil.copy2(source, target)
    (dest / "VERSION").write_text(version + "\n")


def wheel_licenses(wheel: Path, dest: Path) -> None:
    """The license files a wheel carries in its metadata."""
    with zipfile.ZipFile(wheel) as archive:
        for info in archive.infolist():
            parts = info.filename.split("/")
            if len(parts) == 3 and parts[0].endswith(".dist-info") and parts[1] == "licenses" and parts[2]:
                dest.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as source, open(dest / parts[2], "wb") as out:
                    shutil.copyfileobj(source, out)


def keep_licenses(source: Path, names: list, dest: Path) -> None:
    """Copies license files that must exist: a release has to carry them."""
    dest.mkdir(parents=True, exist_ok=True)
    for name in names:
        shutil.copy2(source / name, dest / name)


def notices(version: str, target: str) -> str:
    ffmpeg = ("" if target.startswith("windows") else
              f"  FFmpeg {FFMPEG[0]:<9} LGPL 2.1 or later. Its source is published with every\n"
              f"                   release, as ffmpeg-{FFMPEG[0]}.tar.xz.\n")
    return (f"DQ8Recomp {version}: the other projects in this download\n\n"
            "tools/ and deps/ hold builds of these, each under its own license. The full\n"
            "texts are in the folder named after each one here.\n\n"
            f"  Python {PYTHON_VERSION:<9} PSF License. The python-build-standalone build\n"
            f"                   ({PYTHON_RELEASE}) also links OpenSSL, SQLite, libffi, zlib and others,\n"
            "                   listed with their licenses in that project's full archives:\n"
            "                   https://github.com/astral-sh/python-build-standalone\n"
            f"  CMake {WHEELS['cmake'][0]:<10} BSD 3-Clause, with the libraries it bundles\n"
            f"  Ninja {WHEELS['ninja'][0]:<10} Apache License 2.0\n"
            f"  pkgconf {WHEELS['pkgconf'][0]:<8} ISC License\n"
            f"  SDL3 {SDL3[0]:<11} zlib License\n"
            + ffmpeg +
            "\nsource/ is DQ8Recomp with its submodules, and fetch/ the sources CMake would\n"
            "download for the build; both keep their own license files.\n")


def tools_env(tools: dict, deps: Path, target: str) -> dict:
    env = dict(os.environ)
    paths = [str(tools["cmake_bin"]), str(tools["ninja_bin"]), str(tools["pkgconf"].parent)]
    env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    env["PKG_CONFIG_PATH"] = str(deps / "lib" / "pkgconfig")
    if target.startswith("macos"):
        env["MACOSX_DEPLOYMENT_TARGET"] = MACOS_DEPLOYMENT_TARGET
    return env


def build_sdl3(archive: Path, work: Path, deps: Path, tools: dict, env: dict, jobs: int) -> None:
    source = work / "SDL3-src"
    shutil.rmtree(source, ignore_errors=True)
    untar(archive, work)
    (work / f"SDL3-{SDL3[0]}").rename(source)
    build = work / "SDL3-build"
    run([tools["cmake"], "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_INSTALL_PREFIX={deps}", "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TESTS=OFF",
         "-DSDL_TEST_LIBRARY=OFF", "-DSDL_EXAMPLES=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"], env=env)
    run([tools["cmake"], "--build", build, "--parallel", str(jobs)], env=env)
    run([tools["cmake"], "--install", build], env=env)


def build_ffmpeg(archive: Path, work: Path, deps: Path, target: str, env: dict, jobs: int) -> None:
    source = work / f"ffmpeg-{FFMPEG[0]}"
    shutil.rmtree(source, ignore_errors=True)
    untar(archive, work)
    flags = [f"--prefix={deps}"] + FFMPEG_CONFIGURE
    if target.startswith("macos"):
        # The game finds them through its rpath, wherever the payload lands.
        flags.append("--install-name-dir=@rpath")
    run(["./configure"] + flags, cwd=source, env=env)
    run(["make", f"-j{jobs}"], cwd=source, env=env)
    run(["make", "install"], cwd=source, env=env)


def harvest_fetch(source: Path, work: Path, fetch_dir: Path, tools: dict, deps: Path, env: dict,
                  target: str) -> list:
    """Configures the recompiler and the game once, as the launcher will, and
    keeps the sources FetchContent cloned."""
    recomp = work / "recomp"
    run([tools["cmake"], "-S", source / "thirdparty" / "PS2Recomp", "-B", recomp, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_STUDIO=OFF",
         "-DPS2X_BUILD_TEST=OFF"], env=env)
    # The game configures against a stand-in corpus: what it fetches does not
    # depend on the translated code. Overlays fetch nothing more.
    stub = work / "stub-generated"
    stub.mkdir(parents=True, exist_ok=True)
    (stub / "stub.cpp").write_text("// stand-in for the translated game\n")
    game = work / "game"
    run([tools["cmake"], "-S", source, "-B", game, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DDQ8_LINK_GENERATED=ON", "-DDQ8_LINK_OVERLAYS=OFF", "-DDQ8_GFX_ENABLE_SDLGPU=ON",
         f"-DDQ8_GENERATED_DIR={stub}", f"-DCMAKE_PREFIX_PATH={deps}",
         f"-DPKG_CONFIG_EXECUTABLE={tools['pkgconf']}", "-DPKG_CONFIG_ARGN=--define-prefix",
         f"-DPython3_EXECUTABLE={tools['python']}"], env=env)
    names = []
    for build in (recomp, game):
        for src in sorted((build / "_deps").glob("*-src")):
            if src.name in names:
                continue
            shutil.copytree(src, fetch_dir / src.name, symlinks=True,
                            ignore=shutil.ignore_patterns(".git"))
            names.append(src.name)
    needed = ["glslang-src", "raylib-src"] + (["spirv_cross-src"] if target.startswith("macos") else [])
    missing = [name for name in needed if name not in names]
    if missing:
        sys.exit(f"payload: {', '.join(missing)} were not fetched; a system glslang or spirv-cross "
                 "hid them. Build the payload where neither is installed.")
    return names


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True, help="the payload directory to create")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cache", type=Path, help="where downloads are kept between runs")
    parser.add_argument("--work", type=Path, help="scratch directory for the builds")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--version", help="names the payload; the commit's short hash by default")
    args = parser.parse_args()

    target = host_platform()
    if target not in PYTHON:
        sys.exit(f"payload: no pins for {target}")
    out = args.out.resolve()
    if out.exists():
        shutil.rmtree(out)
    cache = (args.cache or out.parent / "payload-cache").resolve()
    work = (args.work or out.parent / "payload-work").resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    print(f"payload: {target} into {out}", flush=True)
    version = args.version or checkout_version(args.repo.resolve())
    copy_source(args.repo.resolve(), out / "source", version)

    tools_dir = out / "tools"
    licenses = out / "licenses"
    triple, sha = PYTHON[target]
    name = f"cpython-{PYTHON_VERSION}+{PYTHON_RELEASE}-{triple}-install_only_stripped.tar.gz"
    url = (f"https://github.com/astral-sh/python-build-standalone/releases/download/{PYTHON_RELEASE}/"
           + name.replace("+", "%2B"))
    untar(fetch(url, sha, cache), tools_dir)  # unpacks python/
    # The shallowest one is CPython's own; deeper ones belong to bundled packages.
    found = sorted((tools_dir / "python").rglob("LICENSE.txt"), key=lambda path: len(path.parts))
    if not found:
        sys.exit("payload: the Python build has no LICENSE.txt")
    keep_licenses(found[0].parent, [found[0].name], licenses / "Python")
    for tool, (tool_version, prefix, pins) in WHEELS.items():
        path, digest = pins.get(target) or pins["macos"]
        dest = tools_dir / tool / ("" if tool == "cmake" else "bin")
        wheel = fetch(PYPI + path, digest, cache)
        unpack_wheel(wheel, prefix, dest)
        wheel_licenses(wheel, licenses / {"cmake": "CMake", "ninja": "Ninja"}.get(tool, tool))
    # CMake's notices, its own and its bundled libraries', wherever the wheel
    # keeps them (doc/cmake in the macOS one), without the 9 MB manual.
    cmake_notices = sorted((tools_dir / "cmake").rglob("LICENSE.rst"), key=lambda path: len(path.parts))
    if cmake_notices:
        print(f"payload: CMake's notices from {cmake_notices[0].parent.relative_to(out).as_posix()}", flush=True)
        shutil.copytree(cmake_notices[0].parent, licenses / "CMake", dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("*.qch"))
    else:
        print("payload: this CMake wheel has no notices beyond its license", flush=True)
    pkgconf = tools_dir / "pkgconf" / "bin" / exe("pkgconf", target)
    # FindPkgConfig and people look for it under its usual name.
    shutil.copy2(pkgconf, pkgconf.with_name(exe("pkg-config", target)))
    python = tools_dir / "python" / ("python.exe" if target.startswith("windows") else "bin/python3")
    tools = {"cmake_bin": tools_dir / "cmake" / "bin", "cmake": tools_dir / "cmake" / "bin" / exe("cmake", target),
             "ninja_bin": tools_dir / "ninja" / "bin", "pkgconf": pkgconf, "python": python}

    deps = out / "deps"
    env = tools_env(tools, deps, target)
    build_sdl3(fetch(SDL3[1], SDL3[2], cache), work, deps, tools, env, args.jobs)
    keep_licenses(work / "SDL3-src", ["LICENSE.txt"], licenses / "SDL3")
    if not target.startswith("windows"):
        build_ffmpeg(fetch(FFMPEG[1], FFMPEG[2], cache), work, deps, target, env, args.jobs)
        keep_licenses(work / f"ffmpeg-{FFMPEG[0]}", ["COPYING.LGPLv2.1", "LICENSE.md"], licenses / "FFmpeg")
    (licenses / "README.txt").write_text(notices(version, target))
    for unused in ("share/doc", "share/man", "share/ffmpeg"):
        shutil.rmtree(deps / unused, ignore_errors=True)

    fetched = harvest_fetch(out / "source", work, out / "fetch", tools, deps, env, target)
    prune(out, target)

    manifest = {
        "version": version,
        "platform": target,
        "paths": {
            "source": "source", "fetch": "fetch", "deps": "deps",
            "python": str(python.relative_to(out).as_posix()),
            "cmake": str(tools["cmake_bin"].relative_to(out).as_posix()),
            "ninja": str(tools["ninja_bin"].relative_to(out).as_posix()),
            "pkgconf": str(pkgconf.relative_to(out).as_posix()),
        },
        "versions": {"Python": PYTHON_VERSION, "CMake": WHEELS["cmake"][0], "Ninja": WHEELS["ninja"][0],
                     "pkgconf": WHEELS["pkgconf"][0], "SDL3": SDL3[0]}
                    | ({} if target.startswith("windows") else {"FFmpeg": FFMPEG[0]}),
        "fetched": fetched,
    }
    (out / "payload.json").write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.rmtree(work, ignore_errors=True)
    print(f"payload: done, {version}, fetched {len(fetched)} sources", flush=True)


if __name__ == "__main__":
    main()
