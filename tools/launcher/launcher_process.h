// Runs a command line as a child process, through SDL's process API so it
// works the same on every platform, and hands its output over line by line.
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dq8::launcher {

// The environment children get: the launcher's own, with `extraPath` put
// before PATH. A macOS app started from Finder has only the system PATH, which
// leaves Homebrew's tools out.
struct ChildEnvironment {
    std::vector<std::string> extraPath;
    std::vector<std::pair<std::string, std::string>> set;
};

// Runs args[0] (looked up in PATH) and blocks until it exits; every line of
// stdout and stderr goes to onLine. Returns the exit code, or -1 when it
// could not start (error says why). `cancel` kills it.
int runProcess(const std::vector<std::string> &args, const ChildEnvironment &environment,
               const std::function<void(const std::string &)> &onLine, const std::atomic<bool> &cancel,
               std::string &error);

// The first line of `args`'s output, for version probes; empty on failure.
std::string probeOutput(const std::vector<std::string> &args, const ChildEnvironment &environment);

// The directory list PATH would give, with `extra` first.
std::string joinedPath(const std::vector<std::string> &extra);

} // namespace dq8::launcher
