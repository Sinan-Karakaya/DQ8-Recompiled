// What a release carries beside the launcher (tools/launcher/payload.py): the
// source tree, the tools and libraries the build needs, and the sources CMake
// would otherwise clone, so a player needs nothing but a C++ compiler. Absent
// when the launcher runs from a checkout.
#pragma once

#include "launcher_process.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dq8::launcher {

struct Payload {
    std::filesystem::path root;
    std::string version;  // the source tree's git describe
    std::string platform; // "macos-arm64"
    std::filesystem::path source, fetch, deps;
    std::filesystem::path python, cmakeBin, ninjaBin, pkgconf;
    std::vector<std::pair<std::string, std::string>> versions; // {"CMake", "4.4.4"}
    std::vector<std::string> fetched;                          // "glslang-src"
};

// The payload beside the executable (in the app bundle's Resources on macOS).
std::optional<Payload> findPayload();
std::optional<Payload> loadPayload(const std::filesystem::path &root, std::string &error);

// The workspace's copy of the source tree, which builds and runs the game.
std::filesystem::path unpackedSource(const std::filesystem::path &workspace);

// Children find the payload's tools first, and pkg-config its libraries.
ChildEnvironment payloadEnvironment(const Payload &payload, const std::filesystem::path &repo);
// For both CMake configures: the libraries, Python, pkg-config, and every
// fetched source in place of its git clone.
std::vector<std::string> payloadCMakeArgs(const Payload &payload, const std::filesystem::path &repo);

// Copies the source tree, fetched sources and libraries into repo unless this
// version is there already, keeping repo/build. Refuses a folder it did not
// make, such as a git checkout. progress(done, total) in bytes; false cancels.
enum class Unpacked : uint8_t { Copied, AlreadyThere, Failed };
Unpacked unpackPayload(const Payload &payload, const std::filesystem::path &repo,
                       const std::function<bool(uint64_t, uint64_t)> &progress, std::string &error);

} // namespace dq8::launcher
