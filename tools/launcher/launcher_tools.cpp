#include "launcher_tools.h"

#include "launcher_iso.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace dq8::launcher {
namespace {
std::string home() {
    if (const char *value = SDL_getenv("HOME"))
        return value;
    if (const char *value = SDL_getenv("USERPROFILE"))
        return value;
    return {};
}

std::string versionText(const std::vector<int> &version) {
    std::string text;
    for (size_t i = 0; i < version.size(); ++i)
        text += (i ? "." : "") + std::to_string(version[i]);
    return text;
}

ToolCheck probe(const char *name, const char *purpose, const std::vector<std::string> &args,
                const ChildEnvironment &environment, std::initializer_list<int> minimum = {}) {
    ToolCheck check{name, purpose};
    const std::string output = probeOutput(args, environment);
    if (output.empty())
        return check;
    check.found = true;
    const std::vector<int> version = parseVersion(output);
    check.version = version.empty() ? output : versionText(version);
    if (minimum.size() != 0u && !versionAtLeast(version, minimum)) {
        std::string needed;
        for (const int part : minimum)
            needed += (needed.empty() ? "" : ".") + std::to_string(part);
        check.problem = "Version " + needed + " or newer is needed.";
    }
    return check;
}

#if defined(__linux__)
// ID and ID_LIKE from /etc/os-release, which name the package manager.
std::string distribution() {
    std::ifstream file("/etc/os-release");
    std::string line, id, like;
    while (std::getline(file, line)) {
        const auto take = [&](const char *key, std::string &out) {
            const std::string prefix = std::string(key) + "=";
            if (line.rfind(prefix, 0) == 0u) {
                out = line.substr(prefix.size());
                out.erase(std::remove(out.begin(), out.end(), '"'), out.end());
            }
        };
        take("ID", id);
        take("ID_LIKE", like);
    }
    return id + " " + like;
}
#endif
} // namespace

std::vector<int> parseVersion(const std::string &text) {
    std::vector<int> version;
    size_t i = 0;
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i])))
        ++i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        int part = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
            part = part * 10 + (text[i++] - '0');
        version.push_back(part);
        if (i + 1u < text.size() && text[i] == '.' && std::isdigit(static_cast<unsigned char>(text[i + 1u])))
            ++i;
        else
            break;
    }
    return version;
}

bool versionAtLeast(const std::vector<int> &version, std::initializer_list<int> minimum) {
    size_t i = 0;
    for (const int wanted : minimum) {
        const int have = i < version.size() ? version[i] : 0;
        if (have != wanted)
            return have > wanted;
        ++i;
    }
    return true;
}

bool ToolReport::ready() const {
    return std::all_of(tools.begin(), tools.end(), [](const ToolCheck &tool) { return tool.optional || tool.usable(); });
}

std::vector<std::string> extraToolDirs() {
    const std::string user = home();
    std::vector<std::string> dirs;
#if defined(__APPLE__)
    dirs = {"/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin"};
#elif defined(_WIN32)
    dirs = {"C:\\Program Files\\CMake\\bin"};
    if (const char *local = SDL_getenv("LOCALAPPDATA"))
        dirs.push_back(std::string(local) + "\\Microsoft\\WinGet\\Links");
#else
    dirs = {"/usr/local/bin"};
#endif
    if (!user.empty())
        dirs.push_back(pathUtf8(utf8Path(user) / ".local" / "bin"));
    return dirs;
}

ToolReport checkTools(const ChildEnvironment &environment) {
    ToolReport report;
    report.tools.push_back(probe("CMake", "Configures the build", {"cmake", "--version"}, environment, {3, 24}));
    report.tools.push_back(probe("Ninja", "Runs the build", {"ninja", "--version"}, environment));
#if defined(_WIN32)
    ToolCheck python = probe("Python", "Translates the game code", {"py", "-3", "--version"}, environment, {3, 10});
    if (!python.found)
        python = probe("Python", "Translates the game code", {"python", "--version"}, environment, {3, 10});
    report.tools.push_back(python);
    ToolCheck compiler{"Visual Studio C++", "Compiles the game"};
    const std::string vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    const std::string found = probeOutput({vswhere, "-latest", "-products", "*", "-requires",
                                           "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                                           "installationVersion"},
                                          environment);
    compiler.found = !found.empty();
    compiler.version = found;
    report.tools.push_back(compiler);
#else
    report.tools.push_back(
        probe("Python", "Translates the game code", {"python3", "--version"}, environment, {3, 10}));
    report.tools.push_back(probe("C++ compiler", "Compiles the game", {"c++", "--version"}, environment));
    report.tools.push_back(probe("pkg-config", "Finds SDL3 and FFmpeg", {"pkg-config", "--version"}, environment));
    report.tools.push_back(probe("SDL3", "Window, graphics, sound and controllers",
                                 {"pkg-config", "--modversion", "sdl3"}, environment, {3, 2}));
    report.tools.push_back(probe("FFmpeg", "Plays the movies; without it they are skipped",
                                 {"pkg-config", "--modversion", "libavcodec"}, environment));
    report.tools.back().optional = true;
#endif
#if defined(__APPLE__)
    // Homebrew's LLVM stays off the build's PATH, where its clang++ would replace
    // Apple's; CMake finds llvm-ar in these directories by itself.
    ChildEnvironment llvm = environment;
    llvm.extraPath.insert(llvm.extraPath.end(), {"/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin"});
    report.tools.push_back(probe("LLVM", "Archives the large compiled game", {"llvm-ar", "--version"}, llvm));
#endif

    if (std::all_of(report.tools.begin(), report.tools.end(), [](const ToolCheck &tool) { return tool.usable(); }))
        return report;
#if defined(__APPLE__)
    if (probeOutput({"brew", "--version"}, environment).empty()) {
        report.instructions = "Install Homebrew from brew.sh, then come back and check again.";
    } else {
        report.installCommand = "xcode-select --install; brew install cmake ninja pkgconf sdl3 ffmpeg llvm python";
        report.instructions = "Paste this into Terminal, wait for it to finish, then check again.";
    }
#elif defined(_WIN32)
    report.installCommand = "winget install Kitware.CMake Ninja-build.Ninja Python.Python.3.12 "
                            "Microsoft.VisualStudio.2022.BuildTools";
    report.instructions = "Paste this into a terminal. In the Visual Studio installer that follows, pick "
                          "\"Desktop development with C++\". SDL3 and FFmpeg come from vcpkg; see the wiki's "
                          "Building page.";
#else
    const std::string distro = distribution();
    if (distro.find("debian") != std::string::npos || distro.find("ubuntu") != std::string::npos) {
        report.installCommand = "sudo apt install cmake ninja-build python3 g++ pkg-config libsdl3-dev "
                                "libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev "
                                "libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev "
                                "libasound2-dev";
    } else if (distro.find("fedora") != std::string::npos || distro.find("rhel") != std::string::npos) {
        report.installCommand = "sudo dnf install cmake ninja-build python3 gcc-c++ pkgconf SDL3-devel "
                                "ffmpeg-free-devel libX11-devel libXrandr-devel libXinerama-devel "
                                "libXcursor-devel libXi-devel mesa-libGL-devel alsa-lib-devel";
    } else if (distro.find("arch") != std::string::npos) {
        report.installCommand = "sudo pacman -S --needed cmake ninja python gcc pkgconf sdl3 ffmpeg libx11 "
                                "libxrandr libxinerama libxcursor libxi mesa alsa-lib";
    }
    report.instructions = report.installCommand.empty()
                              ? "Install the tools above with your distribution's package manager, then check "
                                "again."
                              : "Paste this into a terminal, then check again.";
#endif
    return report;
}

} // namespace dq8::launcher
