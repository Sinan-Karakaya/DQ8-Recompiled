// What building the game needs on this machine, found or not, with what to
// install when it is missing.
#pragma once

#include "launcher_process.h"

#include <string>
#include <vector>

namespace dq8::launcher {

struct ToolCheck {
    std::string name;    // "CMake"
    std::string purpose; // "Configures the build"
    bool found = false;
    std::string version; // "3.31.6", or what was found instead
    std::string problem; // why it does not do, when found but unsuitable
    bool optional = false;
    bool usable() const { return found && problem.empty(); }
};

struct ToolReport {
    std::vector<ToolCheck> tools;
    // One command that installs everything missing, for this platform; empty
    // when there is none to give (then `instructions` says what to do).
    std::string installCommand;
    std::string instructions;
    bool ready() const;
};

// The directories searched besides PATH: where Homebrew, the CMake and Python
// installers, and user-local installs put tools.
std::vector<std::string> extraToolDirs();

// Runs the version probes; takes a second or two.
ToolReport checkTools(const ChildEnvironment &environment);

// "cmake version 3.31.6" -> {3, 31, 6}; missing parts are 0.
std::vector<int> parseVersion(const std::string &text);
bool versionAtLeast(const std::vector<int> &version, std::initializer_list<int> minimum);

} // namespace dq8::launcher
