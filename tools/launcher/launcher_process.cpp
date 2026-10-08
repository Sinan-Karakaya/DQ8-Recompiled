#include "launcher_process.h"

#include "launcher_iso.h"

#include <filesystem>
#include <system_error>

namespace dq8::launcher {
namespace {
#if defined(_WIN32)
constexpr char kPathSeparator = ';';
#else
constexpr char kPathSeparator = ':';
#endif

// Windows ignores case in variable names and spells its PATH "Path"; SDL's
// environment does not ignore it. Asked for "PATH", SDL finds nothing there,
// and setting "PATH" adds a second variable beside "Path".
std::vector<std::string> spellings(SDL_Environment *env, const std::string &name) {
    std::vector<std::string> names;
    char **variables = SDL_GetEnvironmentVariables(env);
    if (!variables)
        return names;
    for (char **variable = variables; *variable; ++variable) {
        const char *equals = SDL_strchr(*variable, '=');
        if (!equals)
            continue;
        const std::string found(*variable, static_cast<size_t>(equals - *variable));
#if defined(_WIN32)
        if (SDL_strcasecmp(found.c_str(), name.c_str()) == 0)
#else
        if (found == name)
#endif
            names.push_back(found);
    }
    SDL_free(variables);
    return names;
}

const char *variable(SDL_Environment *env, const std::string &name) {
    const std::vector<std::string> names = spellings(env, name);
    return names.empty() ? nullptr : SDL_GetEnvironmentVariable(env, names.front().c_str());
}

// Replaces `name` whatever its spelling, keeping the one already there.
void setVariable(SDL_Environment *env, const std::string &name, const std::string &value) {
    const std::vector<std::string> names = spellings(env, name);
    for (const std::string &old : names)
        SDL_UnsetEnvironmentVariable(env, old.c_str());
    SDL_SetEnvironmentVariable(env, (names.empty() ? name : names.front()).c_str(), value.c_str(), true);
}

// Spawning searches the launcher's own PATH, not the child's, so commands are
// resolved here against the augmented one.
std::string resolve(const std::string &command, const std::vector<std::string> &extraPath) {
    if (command.find('/') != std::string::npos || command.find('\\') != std::string::npos)
        return command;
    const std::string path = joinedPath(extraPath);
#if defined(_WIN32)
    const char *const suffixes[] = {".exe", ".cmd", ".bat", ""};
#else
    const char *const suffixes[] = {""};
#endif
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(kPathSeparator, start);
        if (end == std::string::npos)
            end = path.size();
        const std::string dir = path.substr(start, end - start);
        start = end + 1u;
        if (dir.empty())
            continue;
        for (const char *suffix : suffixes) {
            std::error_code ec;
            const std::filesystem::path candidate = utf8Path(dir) / utf8Path(command + suffix);
            if (std::filesystem::is_regular_file(candidate, ec))
                return pathUtf8(candidate);
        }
    }
    return command;
}

// A plain kill leaves what the child started running: ninja forwards SIGTERM
// to its compilers, and Windows needs taskkill /T for the tree.
void stop(SDL_Process *process, bool force) {
#if defined(_WIN32)
    (void)force;
    const Sint64 pid = SDL_GetNumberProperty(SDL_GetProcessProperties(process), SDL_PROP_PROCESS_PID_NUMBER, 0);
    if (pid > 0) {
        const std::string id = std::to_string(pid);
        const char *args[] = {"taskkill", "/T", "/F", "/PID", id.c_str(), nullptr};
        if (SDL_Process *killer = SDL_CreateProcess(args, false)) {
            SDL_WaitProcess(killer, true, nullptr);
            SDL_DestroyProcess(killer);
        }
    }
    SDL_KillProcess(process, true);
#else
    SDL_KillProcess(process, force);
#endif
}

// Starts args[0] with stdout and stderr going to `output`, or to a pipe the
// caller reads when there is none.
SDL_Process *spawn(const std::vector<std::string> &args, const ChildEnvironment &environment, SDL_IOStream *output,
                   std::string &error) {
    if (args.empty()) {
        error = "Nothing to run.";
        return nullptr;
    }
    std::vector<std::string> resolved = args;
    resolved[0] = resolve(args[0], environment.extraPath);
    std::vector<const char *> argv;
    for (const std::string &arg : resolved)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    SDL_Environment *env = SDL_CreateEnvironment(true);
    if (env) {
        setVariable(env, "PATH", joinedPath(environment.extraPath));
        for (const auto &[name, value] : environment.set)
            setVariable(env, name, value);
    }
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    if (env)
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          output ? SDL_PROCESS_STDIO_REDIRECT : SDL_PROCESS_STDIO_APP);
    if (output)
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, output);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    // The child has its own copy by now.
    if (env)
        SDL_DestroyEnvironment(env);
    if (!process)
        error = "Could not start " + args[0] + ": " + SDL_GetError();
    return process;
}

// Hands each complete line in `pending` to onLine and keeps the unfinished rest.
void takeLines(std::string &pending, const std::function<void(const std::string &)> &onLine) {
    size_t start = 0;
    for (size_t at; (at = pending.find_first_of("\r\n", start)) != std::string::npos; start = at + 1u)
        if (at > start && onLine)
            onLine(pending.substr(start, at - start));
    pending.erase(0, start);
}
} // namespace

std::string joinedPath(const std::vector<std::string> &extra) {
    std::string path;
    for (const std::string &dir : extra)
        path += dir + kPathSeparator;
    if (const char *current = variable(SDL_GetEnvironment(), "PATH"))
        path += current;
    return path;
}

int runProcess(const std::vector<std::string> &args, const ChildEnvironment &environment,
               const std::function<void(const std::string &)> &onLine, const std::atomic<bool> &cancel,
               std::string &error) {
    SDL_Process *process = spawn(args, environment, nullptr, error);
    if (!process)
        return -1;

    SDL_IOStream *output = SDL_GetProcessOutput(process);
    std::string pending;
    char buffer[4096];
    bool killed = false, exited = false;
    Uint64 forceAt = 0u;
    int code = -1;
    while (output) {
        if (cancel.load() && !killed) {
            stop(process, false);
            killed = true;
            forceAt = SDL_GetTicks() + 5000u;
        } else if (forceAt != 0u && SDL_GetTicks() >= forceAt) {
            stop(process, true);
            forceAt = 0u;
        }
        const size_t read = SDL_ReadIO(output, buffer, sizeof(buffer));
        if (read == 0u) {
            // The stream does not block: no data yet is NOT_READY, a closed pipe EOF.
            if (SDL_GetIOStatus(output) != SDL_IO_STATUS_NOT_READY)
                break;
            // Drained after the child exited: something it started may still
            // hold the pipe open, so stop waiting for EOF.
            if (exited)
                break;
            exited = SDL_WaitProcess(process, false, &code);
            if (!exited)
                SDL_Delay(5);
            continue;
        }
        pending.append(buffer, read);
        takeLines(pending, onLine);
    }
    if (!pending.empty() && onLine)
        onLine(pending);
    if (!exited)
        SDL_WaitProcess(process, true, &code);
    SDL_DestroyProcess(process);
    if (killed) {
        error = "Cancelled.";
        return -1;
    }
    return code;
}

int runProcessToFile(const std::vector<std::string> &args, const ChildEnvironment &environment,
                     const std::filesystem::path &log, const std::function<void(const std::string &)> &onLine,
                     const std::atomic<bool> &stop, std::string &error) {
    std::error_code ec;
    if (log.has_parent_path())
        std::filesystem::create_directories(log.parent_path(), ec);
    const std::string logPath = pathUtf8(log);
    SDL_IOStream *file = SDL_IOFromFile(logPath.c_str(), "w");
    if (!file) {
        error = "Could not write " + logPath + ": " + SDL_GetError();
        return -1;
    }
    SDL_Process *process = spawn(args, environment, file, error);
    // The child holds its own handle to the file.
    SDL_CloseIO(file);
    if (!process)
        return -1;

    std::string pending;
    char buffer[4096];
    Sint64 offset = 0;
    int code = -1;
    for (bool exited = false; !exited;) {
        if (stop.load()) {
            // Neither kills nor waits for the child.
            SDL_DestroyProcess(process);
            error = "Stopped watching.";
            return -1;
        }
        // Read after the check, so that the last pass sees all the child wrote.
        exited = SDL_WaitProcess(process, false, &code);
        // Windows keeps the file locked while the child writes it: there the
        // lines arrive once it exits.
        if (SDL_IOStream *reader = SDL_IOFromFile(logPath.c_str(), "rb")) {
            if (SDL_SeekIO(reader, offset, SDL_IO_SEEK_SET) == offset) {
                for (size_t read; (read = SDL_ReadIO(reader, buffer, sizeof(buffer))) > 0u;) {
                    pending.append(buffer, read);
                    offset += static_cast<Sint64>(read);
                }
            }
            SDL_CloseIO(reader);
        }
        takeLines(pending, onLine);
        if (!exited)
            SDL_Delay(100);
    }
    if (!pending.empty() && onLine)
        onLine(pending);
    SDL_DestroyProcess(process);
    return code;
}

std::string probeOutput(const std::vector<std::string> &args, const ChildEnvironment &environment) {
    std::string first, error;
    const std::atomic<bool> never{false};
    const int code = runProcess(
        args, environment,
        [&](const std::string &line) {
            if (first.empty() && line.find_first_not_of(" \t") != std::string::npos)
                first = line;
        },
        never, error);
    return code == 0 ? first : std::string();
}

} // namespace dq8::launcher
