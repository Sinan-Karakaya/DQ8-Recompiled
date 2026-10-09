#include "launcher_payload.h"

#include "launcher_iso.h"
#include "launcher_json.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace dq8::launcher {
namespace {
// In the unpacked folder: the launcher made it, so it may replace its contents.
constexpr const char *kOwnerMarker = ".dq8recomp-unpacked";
// In its build/: which payload version it holds.
constexpr const char *kVersionMarker = "payload-unpacked.txt";
// The Tools page lists them in this order.
constexpr const char *kToolOrder[] = {"Python", "CMake", "Ninja", "pkgconf", "SDL3", "FFmpeg"};
#if defined(_WIN32)
constexpr const char *kNinja = "ninja.exe";
#else
constexpr const char *kNinja = "ninja";
#endif

std::string readFile(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// CMake reads backslashes in a -D value as escapes in some places.
std::string cmakePath(const std::filesystem::path &path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

uint64_t treeBytes(const std::filesystem::path &dir) {
    uint64_t bytes = 0u;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entryError;
        if (it->is_regular_file(entryError))
            bytes += it->file_size(entryError);
    }
    return bytes;
}

bool copyTree(const std::filesystem::path &from, const std::filesystem::path &to, uint64_t &done, uint64_t total,
              const std::function<bool(uint64_t, uint64_t)> &progress, std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::filesystem::path target = to / it->path().lexically_relative(from);
        std::error_code entryError;
        if (it->is_symlink(entryError)) {
            std::filesystem::remove(target, entryError);
            std::filesystem::copy_symlink(it->path(), target, entryError);
        } else if (it->is_directory(entryError)) {
            std::filesystem::create_directories(target, entryError);
        } else {
            std::filesystem::copy_file(it->path(), target, std::filesystem::copy_options::overwrite_existing,
                                       entryError);
            done += entryError ? 0u : it->file_size(entryError);
        }
        if (entryError) {
            error = "Cannot copy " + pathUtf8(target) + ": " + entryError.message();
            return false;
        }
        if (progress && !progress(done, total)) {
            error = "Cancelled.";
            return false;
        }
    }
    if (ec) {
        error = "Cannot read " + pathUtf8(from) + ": " + ec.message();
        return false;
    }
    return true;
}
} // namespace

std::optional<Payload> loadPayload(const std::filesystem::path &root, std::string &error) {
    const std::string text = readFile(root / "payload.json");
    JsonValue manifest;
    if (text.empty()) {
        error = "There is no payload.json in " + pathUtf8(root) + ".";
        return std::nullopt;
    }
    if (!parseJson(text, manifest, error))
        return std::nullopt;
    Payload payload;
    payload.root = root;
    payload.version = manifest["version"].string;
    payload.platform = manifest["platform"].string;
    const JsonValue &paths = manifest["paths"];
    const auto path = [&](const char *key) { return root / utf8Path(paths[key].string); };
    payload.source = path("source");
    payload.fetch = path("fetch");
    payload.deps = path("deps");
    payload.python = path("python");
    payload.cmakeBin = path("cmake");
    payload.ninjaBin = path("ninja");
    payload.pkgconf = path("pkgconf");
    // An antivirus may have taken it away since the download.
    std::error_code recompilerError;
    if (!paths["recompiler"].string.empty() && std::filesystem::is_regular_file(path("recompiler"), recompilerError))
        payload.recompiler = path("recompiler");
    for (const char *name : kToolOrder) {
        const JsonValue &version = manifest["versions"][name];
        if (version.kind == JsonValue::Kind::String)
            payload.versions.emplace_back(name, version.string);
    }
    for (const JsonValue &name : manifest["fetched"].array)
        payload.fetched.push_back(name.string);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(payload.source / "setup.py", ec) ||
        !std::filesystem::exists(payload.python, ec) || !std::filesystem::is_directory(payload.cmakeBin, ec)) {
        error = "The payload in " + pathUtf8(root) + " is incomplete.";
        return std::nullopt;
    }
    return payload;
}

std::optional<Payload> findPayload() {
    // The executable's folder, or the bundle's Resources on macOS.
    const char *base = SDL_GetBasePath();
    if (!base)
        return std::nullopt;
    std::string error;
    return loadPayload(utf8Path(base) / "payload", error);
}

std::filesystem::path unpackedSource(const std::filesystem::path &workspace) { return workspace / "DQ8Recomp-source"; }

ChildEnvironment payloadEnvironment(const Payload &payload, const std::filesystem::path &repo) {
    ChildEnvironment environment;
    environment.extraPath = {pathUtf8(payload.python.parent_path()), pathUtf8(payload.cmakeBin),
                             pathUtf8(payload.ninjaBin), pathUtf8(payload.pkgconf.parent_path())};
    environment.set.emplace_back("PKG_CONFIG_PATH", pathUtf8(repo / "build" / "deps" / "lib" / "pkgconfig"));
    return environment;
}

std::vector<std::string> payloadCMakeArgs(const Payload &payload, const std::filesystem::path &repo) {
    const std::filesystem::path build = repo / "build";
    std::vector<std::string> args = {
        "-DCMAKE_PREFIX_PATH=" + cmakePath(build / "deps"),
        // Cached otherwise: unpacking a newer release elsewhere left CMake
        // running the ninja of a folder that was gone.
        "-DCMAKE_MAKE_PROGRAM=" + cmakePath(payload.ninjaBin / kNinja),
        "-DPKG_CONFIG_EXECUTABLE=" + cmakePath(payload.pkgconf),
        // The .pc files name the machine the payload was built on.
        "-DPKG_CONFIG_ARGN=--define-prefix",
        "-DPython3_EXECUTABLE=" + cmakePath(payload.python),
#if defined(__APPLE__)
        // A release builds the same on every Mac: none of Homebrew's libraries
        // or its keg-only LLVM, which find_program would reach by name.
        "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local",
        "-DCMAKE_IGNORE_PATH=/opt/homebrew/opt/llvm/bin;/usr/local/opt/llvm/bin",
#endif
    };
    for (const std::string &name : payload.fetched) {
        if (name.size() <= 4u || !name.ends_with("-src"))
            continue;
        // "spirv_cross-src" holds FetchContent's spirv_cross.
        std::string upper = name.substr(0, name.size() - 4u);
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        args.push_back("-DFETCHCONTENT_SOURCE_DIR_" + upper + "=" + cmakePath(build / "fetch" / name));
    }
    return args;
}

Unpacked unpackPayload(const Payload &payload, const std::filesystem::path &repo,
                       const std::function<bool(uint64_t, uint64_t)> &progress, std::string &error) {
    std::error_code ec;
    const std::filesystem::path build = repo / "build";
    const bool owned = std::filesystem::exists(repo / kOwnerMarker, ec);
    if (owned && readFile(build / kVersionMarker) == payload.version + "\n" &&
        std::filesystem::is_regular_file(repo / "setup.py", ec))
        return Unpacked::AlreadyThere;
    // Never empty a folder the launcher did not make, a checkout above all.
    if (!owned && std::filesystem::exists(repo, ec) && !std::filesystem::is_empty(repo, ec)) {
        error = pathUtf8(repo) + " already exists and was not made by the launcher. Choose another folder for the "
                                 "game files in Options.";
        return Unpacked::Failed;
    }
    // A new version replaces the source tree; build/ stays, so the next build
    // only redoes what changed.
    for (const auto &entry : std::filesystem::directory_iterator(repo, ec)) {
        std::error_code removeError;
        if (entry.path().filename() != "build")
            std::filesystem::remove_all(entry.path(), removeError);
    }
    std::filesystem::remove_all(build / "fetch", ec);
    std::filesystem::remove_all(build / "deps", ec);
    std::filesystem::remove(build / kVersionMarker, ec);
    std::filesystem::create_directories(build, ec);
    if (ec) {
        error = "Cannot create " + pathUtf8(build) + ": " + ec.message();
        return Unpacked::Failed;
    }
    std::ofstream(repo / kOwnerMarker, std::ios::binary)
        << "Made by the DQ8Recomp launcher, which replaces this folder's contents when it updates.\n";
    const uint64_t total = treeBytes(payload.source) + treeBytes(payload.fetch) + treeBytes(payload.deps);
    uint64_t done = 0u;
    if (!copyTree(payload.source, repo, done, total, progress, error) ||
        !copyTree(payload.fetch, build / "fetch", done, total, progress, error) ||
        !copyTree(payload.deps, build / "deps", done, total, progress, error))
        return Unpacked::Failed;
    std::ofstream(build / kVersionMarker, std::ios::binary | std::ios::trunc) << payload.version << "\n";
    return Unpacked::Copied;
}

} // namespace dq8::launcher
