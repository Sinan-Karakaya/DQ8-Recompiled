#include "launcher_pipeline.h"

#include "launcher_iso.h"
#include "launcher_json.h"
#include "launcher_sha256.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>

namespace dq8::launcher {
namespace {
constexpr const char *kVersion = "SLUS_212.07";
constexpr size_t kLogLines = 4000u;
using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
constexpr const char *kExe = ".exe";
#else
constexpr const char *kExe = "";
#endif

std::string readText(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool loadHashes(const std::filesystem::path &repo, JsonValue &out, std::string &error) {
    const std::filesystem::path path = repo / "config" / kVersion / "hashes.json";
    const std::string text = readText(path);
    if (text.empty()) {
        error = "The source tree has no " + pathUtf8(path) + ".";
        return false;
    }
    return parseJson(text, out, error);
}

std::string thousands(uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), ",");
    return digits;
}

std::string gigabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

std::vector<std::string> pythonCommand() {
#if defined(_WIN32)
    return {"py", "-3"};
#else
    return {"python3"};
#endif
}

// Hashes a file in chunks; progress(done, total) returning false stops it.
std::string hashFile(const std::filesystem::path &path, const std::function<bool(uint64_t, uint64_t)> &progress) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::error_code ec;
    const uint64_t total = std::filesystem::file_size(path, ec);
    constexpr size_t kChunk = 8u << 20;
    std::unique_ptr<char[]> buffer(new char[kChunk]);
    Sha256 sha;
    uint64_t done = 0u;
    while (file) {
        file.read(buffer.get(), kChunk);
        const auto got = static_cast<size_t>(file.gcount());
        if (got == 0u)
            break;
        sha.update(buffer.get(), got);
        done += got;
        if (progress && !progress(done, total))
            return {};
    }
    return sha.finishHex();
}

// The recompiler reports ~32k unhandled instructions on a healthy run; they
// belong in the log, not in the status line where they read as failure, as
// does ninja's own chatter.
bool diagnostic(const std::string &text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return true;
    for (const std::string_view prefix : {"[error]", "[warning]", "Unknown ", "ninja: "})
        if (std::string_view(text).substr(first).starts_with(prefix))
            return true;
    return false;
}

// What translating reads. When none of it changed the corpus on disk is
// current, and translating again would rewrite all 12k files, so ninja would
// compile every one of them again.
std::string translationInputs(const std::filesystem::path &repo, const std::filesystem::path &extracted,
                              const JsonValue &hashes) {
    std::vector<std::filesystem::path> files = {recompilerPath(repo), repo / "setup.py",
                                                repo / "thirdparty" / "PS2Recomp" / "tools" /
                                                    "vu_program_manifest.py"};
    for (const auto &[dir, pythonOnly] : {std::pair{repo / "config" / kVersion, false},
                                          std::pair{repo / "tools" / "mwo3", true}}) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && (!pythonOnly || it->path().extension() == ".py"))
                files.push_back(it->path());
    }
    for (const auto &[name, entry] : hashes["files"].object)
        files.push_back(extracted / utf8Path(name));
    std::sort(files.begin(), files.end());
    std::string text;
    for (const std::filesystem::path &file : files) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        const auto time = static_cast<long long>(std::filesystem::last_write_time(file, ec).time_since_epoch().count());
        text += pathUtf8(file) + "\n" + std::to_string(size) + " " + std::to_string(time) + "\n";
    }
    return text;
}

#if defined(_WIN32)
// MSVC's tools need vcvars64.bat's environment, so CMake runs from a batch
// file that calls it first; quoting through cmd.exe directly is fragile.
std::vector<std::string> withCompilerEnvironment(const std::vector<std::string> &args,
                                                 const std::filesystem::path &workspace,
                                                 const ChildEnvironment &environment) {
    const std::string install =
        probeOutput({"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe", "-latest",
                     "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                     "-property", "installationPath"},
                    environment);
    if (install.empty())
        return args;
    const std::filesystem::path script = workspace / "launcher-step.bat";
    // Binary: the lines carry their own \r\n, which text mode would double.
    std::ofstream bat(script, std::ios::binary | std::ios::trunc);
    bat << "@echo off\r\ncall \"" << install << "\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul || exit /b 1\r\n";
    // Without a Windows SDK, CMake finds cl.exe but not rc.exe, and its first
    // test program fails to link without saying why.
    bat << "if not defined WindowsSdkDir (\r\n"
           "  echo Visual Studio has no Windows SDK: add one in the Visual Studio Installer, then build again.\r\n"
           "  exit /b 1\r\n)\r\n";
    for (const std::string &arg : args)
        bat << '"' << arg << "\" ";
    bat << "\r\nexit /b %ERRORLEVEL%\r\n";
    return {"cmd.exe", "/d", "/c", pathUtf8(script)};
}
#endif
} // namespace

const char *stageTitle(Stage stage) {
    switch (stage) {
    case Stage::CheckDisc: return "Check the disc";
    case Stage::ExtractDisc: return "Copy the disc's files";
    case Stage::UnpackSource: return "Unpack DQ8Recomp";
    case Stage::BuildRecompiler: return "Build the recompiler";
    case Stage::TranslateGame: return "Translate the game";
    case Stage::ConfigureGame: return "Prepare the build";
    case Stage::CompileGame: return "Compile the game";
    case Stage::Count: break;
    }
    return "";
}

const char *statusName(StageState::Status status) {
    switch (status) {
    case StageState::Status::Waiting: return "waiting";
    case StageState::Status::Running: return "running";
    case StageState::Status::Done: return "done";
    case StageState::Status::Failed: return "failed";
    case StageState::Status::Skipped: return "already done";
    }
    return "";
}

std::filesystem::path recompilerPath(const std::filesystem::path &repo) {
    return repo / "build" / "ps2recomp-standalone" / "ps2xRecomp" / (std::string("ps2_recomp") + kExe);
}

std::filesystem::path gamePath(const std::filesystem::path &repo) {
    return repo / "build" / "game" / "src" / "runtime" / (std::string("dq8") + kExe);
}

std::filesystem::path extractedDisc(const std::filesystem::path &workspace) { return workspace / "Extracted_Usa"; }

namespace {
std::filesystem::path builtMarker(const std::filesystem::path &repo) {
    return repo / "build" / "game" / "launcher-built.txt";
}

std::string builtText(const BuiltFrom &from) {
    return pathUtf8(from.workspace) + "\n" + from.version + "\n" + pathUtf8(from.disc) + "\n";
}
} // namespace

void markBuilt(const std::filesystem::path &repo, const BuiltFrom &from) {
    std::ofstream(builtMarker(repo), std::ios::binary | std::ios::trunc) << builtText(from);
}

bool launcherBuilt(const std::filesystem::path &repo, const BuiltFrom &from) {
    std::string marker = readText(builtMarker(repo));
    // A marker written in text mode on Windows has \r\n line ends.
    std::erase(marker, '\r');
    std::error_code ec;
    return marker == builtText(from) && std::filesystem::is_regular_file(gamePath(repo), ec) &&
           std::filesystem::is_regular_file(extractedDisc(from.workspace) / kVersion, ec);
}

bool configuredWith(const std::filesystem::path &build, const std::string &compiler) {
    std::ifstream cache(build / "CMakeCache.txt");
    int named = 0;
    for (std::string line; std::getline(cache, line);) {
        if (!line.starts_with("CMAKE_C_COMPILER:") && !line.starts_with("CMAKE_CXX_COMPILER:"))
            continue;
        const std::filesystem::path path = utf8Path(line.substr(line.find('=') + 1));
        std::error_code ec;
        if (!path.is_absolute() || !std::filesystem::is_regular_file(path, ec) ||
            SDL_strcasecmp(pathUtf8(path.stem()).c_str(), compiler.c_str()) != 0)
            return false;
        ++named;
    }
    return named == 2;
}

double compileCost(const std::string &file, uint64_t bytes) {
    // Tables such as register_functions.cpp are as large but quick.
    if (file.rfind("FUN_", 0) != 0u && file.rfind("gap_", 0) != 0u)
        return 1.0;
    const double scaled = static_cast<double>(bytes) / 450e3;
    return 1.0 + scaled * scaled * scaled;
}

std::string generatedKey(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    constexpr std::string_view kMarker = "build/generated/";
    const size_t at = path.rfind(kMarker);
    if (at == std::string::npos)
        return {};
    std::string key = path.substr(at + kMarker.size());
    for (const std::string_view suffix : {".obj", ".o"}) {
        if (key.size() > suffix.size() && key.ends_with(suffix)) {
            key.resize(key.size() - suffix.size());
            break;
        }
    }
    return key;
}

void CompilePlan::scan(const std::filesystem::path &generated, const std::filesystem::path &buildDir) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(generated, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError) || it->path().extension() != ".cpp")
            continue;
        File file;
        file.cost = compileCost(pathUtf8(it->path().filename()), it->file_size(entryError));
        files[generatedKey(pathUtf8(std::filesystem::path("build/generated") /
                                    it->path().lexically_relative(generated)))] = file;
        total += file.cost;
    }
    // Compiled already: an object at least as new as its source.
    for (auto it = std::filesystem::recursive_directory_iterator(buildDir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const auto extension = it->path().extension();
        if (extension != ".o" && extension != ".obj")
            continue;
        const auto found = files.find(generatedKey(pathUtf8(it->path().lexically_relative(buildDir))));
        if (found == files.end() || found->second.compiled)
            continue;
        std::error_code timeError;
        const auto objectTime = it->last_write_time(timeError);
        const auto sourceTime = std::filesystem::last_write_time(generated / utf8Path(found->first), timeError);
        if (!timeError && objectTime >= sourceTime) {
            found->second.compiled = true;
            compiled += found->second.cost;
        }
    }
    for (const auto &[key, file] : files) {
        if (file.compiled)
            continue;
        ++pending;
        largeLeft += file.cost >= kLargeCompileCost ? 1u : 0u;
    }
}

bool CompilePlan::finish(const std::string &description) {
    const auto found = files.find(generatedKey(description));
    if (found == files.end())
        return false;
    if (!found->second.compiled) {
        found->second.compiled = true;
        compiled += found->second.cost;
        --pending;
        largeLeft -= found->second.cost >= kLargeCompileCost ? 1u : 0u;
    }
    return true;
}

void CompilePlan::forgetCompiled() {
    compiled = 0.0;
    pending = files.size();
    largeLeft = 0u;
    for (auto &[key, file] : files) {
        file.compiled = false;
        largeLeft += file.cost >= kLargeCompileCost ? 1u : 0u;
    }
}

bool parseNinjaProgress(const std::string &line, uint64_t &done, uint64_t &total) {
    if (line.size() < 5u || line[0] != '[')
        return false;
    char *end = nullptr;
    const unsigned long long first = std::strtoull(line.c_str() + 1, &end, 10);
    if (end == line.c_str() + 1 || *end != '/')
        return false;
    const char *second = end + 1;
    const unsigned long long last = std::strtoull(second, &end, 10);
    if (end == second || *end != ']')
        return false;
    done = first;
    total = last;
    return true;
}

DiscInfo inspectDisc(const std::filesystem::path &repo, const std::filesystem::path &disc) {
    DiscInfo info;
    IsoImage image;
    std::string error;
    if (!image.open(pathUtf8(disc), error)) {
        info.error = error;
        return info;
    }
    info.readable = true;
    info.volume = image.volumeId();
    info.size = image.imageSize();
    std::string cnf;
    if (const IsoEntry *entry = image.find("SYSTEM.CNF"); entry && image.read(*entry, cnf, error))
        info.executable = bootExecutable(cnf);
    JsonValue hashes;
    if (!loadHashes(repo, hashes, error)) {
        info.error = error;
        return info;
    }
    const auto expected = static_cast<uint64_t>(hashes["iso"]["size"].number);
    info.supported = info.executable == kVersion && info.size == expected;
    if (info.executable.empty())
        info.error = "This disc image has no PlayStation 2 boot file.";
    else if (info.executable != kVersion)
        info.error = "This is " + info.executable + ". DQ8Recomp needs the North American release, SLUS_212.07.";
    else if (!info.supported)
        info.error = "This image is not the size of the known-good dump, so it may be damaged or modified.";
    return info;
}

Pipeline::~Pipeline() {
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

void Pipeline::start(const PipelineOptions &options, const ChildEnvironment &environment) {
    if (m_running.load())
        return;
    if (m_thread.joinable())
        m_thread.join();
    {
        std::lock_guard lock(m_mutex);
        m_stages = {};
        m_log.clear();
        m_error.clear();
    }
    ++m_logVersion;
    m_cancel = false;
    m_succeeded = false;
    m_running = true;
    m_thread = std::thread(&Pipeline::run, this, options, environment);
}

void Pipeline::cancel() { m_cancel = true; }

std::array<StageState, kStageCount> Pipeline::stages() const {
    std::lock_guard lock(m_mutex);
    return m_stages;
}

std::vector<std::string> Pipeline::log(size_t lines) const {
    std::lock_guard lock(m_mutex);
    const size_t first = m_log.size() > lines ? m_log.size() - lines : 0u;
    return {m_log.begin() + static_cast<std::ptrdiff_t>(first), m_log.end()};
}

std::string Pipeline::error() const {
    std::lock_guard lock(m_mutex);
    return m_error;
}

void Pipeline::set(Stage stage, double progress, const std::string &detail) {
    std::lock_guard lock(m_mutex);
    StageState &state = m_stages[static_cast<size_t>(stage)];
    state.progress = progress;
    state.detail = detail;
}

void Pipeline::skip(Stage stage, const std::string &detail) {
    std::lock_guard lock(m_mutex);
    StageState &state = m_stages[static_cast<size_t>(stage)];
    state.status = StageState::Status::Skipped;
    state.detail = detail;
}

void Pipeline::line(const std::string &text) {
    std::lock_guard lock(m_mutex);
    m_log.push_back(text);
    if (m_log.size() > kLogLines)
        m_log.pop_front();
    ++m_logVersion;
    if (!m_logFile.empty()) {
        std::ofstream file(m_logFile, std::ios::app);
        file << text << '\n';
    }
}

bool Pipeline::runStage(Stage stage, const std::function<bool()> &body) {
    if (m_cancel.load())
        return false;
    const auto index = static_cast<size_t>(stage);
    {
        std::lock_guard lock(m_mutex);
        m_stages[index].status = StageState::Status::Running;
        m_stages[index].startedMs = SDL_GetTicks();
    }
    line(std::string("== ") + stageTitle(stage));
    const auto started = Clock::now();
    const bool ok = body();
    std::lock_guard lock(m_mutex);
    StageState &state = m_stages[index];
    state.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    state.remaining = -1.0;
    if (ok) {
        if (state.status == StageState::Status::Running)
            state.status = StageState::Status::Done;
        state.progress = 1.0;
    } else {
        state.status = StageState::Status::Failed;
        if (m_error.empty())
            m_error = m_cancel.load() ? "Cancelled." : std::string(stageTitle(stage)) + " failed; see the log.";
    }
    return ok;
}

bool Pipeline::command(Stage stage, const std::vector<std::string> &args, CompilePlan *plan,
                       const std::function<void(const std::string &)> &watch) {
    std::string shown = "$";
    for (const std::string &arg : args)
        shown += " " + arg;
    line(shown);
    std::vector<std::string> run = args;
#if defined(_WIN32)
    if (!args.empty() && (args[0] == "cmake" || args[0] == "ninja"))
        run = withCompilerEnvironment(args, m_logFile.parent_path(), m_environment);
#endif
    // The rate is measured from a mark: the first step, the step 5% in (past
    // the precompiled headers and slowest libraries), then each large step,
    // after which only small ones remain at a steady pace.
    auto mark = Clock::now();
    uint64_t markDone = 0u, lastTotal = 0u;
    bool counting = false, upToDate = false, planChecked = false, warmedUp = false;
    // Steps other than translated files (libraries, links) count one unit each.
    double otherDone = 0.0;
    size_t pendingAtStart = plan ? plan->pending : 0u;
    std::string error;
    const int code = runProcess(
        run, m_environment,
        [&](const std::string &text) {
            line(text);
            if (watch)
                watch(text);
            if (text == "ninja: no work to do.")
                upToDate = true;
            uint64_t done = 0u, total = 0u;
            if (!parseNinjaProgress(text, done, total) || total == 0u) {
                std::lock_guard lock(m_mutex);
                StageState &state = m_stages[static_cast<size_t>(stage)];
                if (state.progress < 0.0 && !diagnostic(text))
                    state.detail = text.size() > 96u ? text.substr(0, 93u) + "..." : text;
                return;
            }
            const auto now = Clock::now();
            if (!counting) {
                counting = true;
                mark = now;
                markDone = done;
            }
            if (!warmedUp && total > 2u && done * 20u >= total) {
                warmedUp = true;
                mark = now;
                markDone = done;
            }
            lastTotal = total;
            size_t largeLeft = 0u;
            double progress = static_cast<double>(done) / static_cast<double>(total);
            if (plan) {
                // ninja's own count says when a header change rebuilds files
                // whose objects look current (its first lines are CMake's).
                if (!planChecked && total > 2u) {
                    planChecked = true;
                    if (total > pendingAtStart + 2000u) {
                        plan->forgetCompiled();
                        pendingAtStart = plan->pending;
                    }
                }
                // Into a pipe, ninja prints a step's line when it finishes (only
                // a terminal gets it at the start too), so this step is done.
                const size_t close = text.find("] ");
                const size_t largeBefore = plan->largeLeft;
                if (close == std::string::npos || !plan->finish(text.substr(close + 2u)))
                    otherDone += 1.0;
                if (plan->largeLeft < largeBefore) {
                    mark = now;
                    markDone = done;
                }
                largeLeft = plan->largeLeft;
                const double others = std::max(otherDone, static_cast<double>(total) - static_cast<double>(pendingAtStart));
                progress = (plan->compiled + otherDone) / (plan->total + others);
            }
            const double elapsed = std::chrono::duration<double>(now - mark).count();
            const double rate = elapsed > 5.0 ? static_cast<double>(done - markDone) / elapsed : 0.0;
            std::lock_guard lock(m_mutex);
            StageState &state = m_stages[static_cast<size_t>(stage)];
            state.progress = std::min(1.0, progress);
            state.detail = thousands(done) + " of " + thousands(total);
            if (largeLeft > 0u)
                state.detail += "  " + std::to_string(largeLeft) + (largeLeft == 1u ? " large file" : " large files") +
                                " left";
            // A count says nothing about how long the large files take.
            state.remaining =
                rate > 0.0 && largeLeft == 0u && warmedUp ? static_cast<double>(total - done) / rate : -1.0;
        },
        m_cancel, error);
    if (code == 0) {
        // The last line of a build is "[122/123] Linking..."; say what it means.
        if (upToDate || counting)
            set(stage, 1.0, upToDate ? "Up to date" : thousands(lastTotal) + " of " + thousands(lastTotal));
        return true;
    }
    std::lock_guard lock(m_mutex);
    if (m_error.empty())
        m_error = !error.empty() ? error : std::string(stageTitle(stage)) + " failed; the log shows why.";
    return false;
}

void Pipeline::run(PipelineOptions options, ChildEnvironment environment) {
    m_environment = std::move(environment);
    // The progress bar reads ninja's "[done/total]" prefix, whatever the user's taste.
    m_environment.set.emplace_back("NINJA_STATUS", "[%f/%t] ");
    std::error_code ec;
    std::filesystem::create_directories(options.workspace, ec);
    m_logFile = options.workspace / "launcher.log";
    std::filesystem::remove(m_logFile, ec);
    const std::filesystem::path repo = options.repo;
    // Play waits for this build; a failed one leaves nothing to start.
    std::filesystem::remove(builtMarker(repo), ec);
    const std::filesystem::path extracted = extractedDisc(options.workspace);
    const std::string jobs = std::to_string(std::max(1, options.jobs));
    const std::optional<Payload> &payload = options.payload;
    // Both CMake configures use the payload's libraries and fetched sources.
    const std::vector<std::string> payloadArgs = payload ? payloadCMakeArgs(*payload, repo) : std::vector<std::string>{};
    const auto withConfigureArgs = [&]([[maybe_unused]] const std::filesystem::path &build,
                                       std::vector<std::string> args) {
#if defined(_WIN32)
        // Visual Studio's compiler, which Tools checks for. Otherwise the game's
        // CMake prefers any clang on PATH, and CMake a MinGW g++: an older clang
        // fails on Visual Studio's headers, a MinGW one on the payload's libraries.
        // A build made with another compiler, or with a cl an update removed,
        // starts over: CMake would switch compilers itself, then configure a
        // second time without any of these -D.
        if (!configuredWith(build, "cl"))
            args.insert(args.end(), {"--fresh", "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"});
#endif
        args.insert(args.end(), payloadArgs.begin(), payloadArgs.end());
        return args;
    };
    JsonValue hashes;
    std::string error;
    // A payload's tree is not unpacked yet; its hashes are the same.
    if (!loadHashes(payload ? payload->source : repo, hashes, error)) {
        std::lock_guard lock(m_mutex);
        m_error = error;
        m_running = false;
        return;
    }

    const bool ok =
        // Every build hashes the whole image again: metadata cannot prove the
        // bytes are the ones checked last time, and it costs seconds.
        runStage(Stage::CheckDisc, [&] {
            uint64_t checked = 0u;
            const std::string sha = hashFile(options.disc, [&](uint64_t done, uint64_t total) {
                set(Stage::CheckDisc, total ? double(done) / double(total) : 0.0,
                    gigabytes(done) + " of " + gigabytes(total));
                checked = total;
                return !m_cancel.load();
            });
            if (sha.empty())
                return false;
            if (sha != hashes["iso"]["sha256"].string) {
                std::lock_guard lock(m_mutex);
                m_error = "The disc image does not match the known-good dump. It may be damaged: dump the "
                          "disc again and retry.";
                return false;
            }
            set(Stage::CheckDisc, 1.0, gigabytes(checked) + " checked");
            return true;
        }) &&
        runStage(Stage::ExtractDisc, [&] {
            // Already there from an earlier run when every listed file has its size.
            bool present = true;
            for (const auto &[name, entry] : hashes["files"].object) {
                std::error_code sizeError;
                if (std::filesystem::file_size(extracted / utf8Path(name), sizeError) !=
                    static_cast<uint64_t>(entry["size"].number))
                    present = false;
            }
            uint64_t copied = 0u;
            if (!present) {
                IsoImage image;
                if (!image.open(pathUtf8(options.disc), error) ||
                    !image.extract(extracted,
                                   [&](uint64_t done, uint64_t total) {
                                       set(Stage::ExtractDisc, total ? double(done) / double(total) : 0.0,
                                           gigabytes(done) + " of " + gigabytes(total));
                                       copied = total;
                                       return !m_cancel.load();
                                   },
                                   error)) {
                    std::lock_guard lock(m_mutex);
                    m_error = error;
                    return false;
                }
            }
            // The small files are hashed too: they are what gets translated.
            for (const auto &[name, entry] : hashes["files"].object) {
                if (entry["size"].number > 64e6)
                    continue;
                if (hashFile(extracted / utf8Path(name), {}) != entry["sha256"].string) {
                    std::lock_guard lock(m_mutex);
                    m_error = name + " differs from the known-good dump.";
                    return false;
                }
            }
            if (!present) {
                set(Stage::ExtractDisc, 1.0, gigabytes(copied) + " copied");
                return true;
            }
            skip(Stage::ExtractDisc, "Copied before");
            return true;
        }) &&
        runStage(Stage::UnpackSource, [&] {
            if (!payload) {
                skip(Stage::UnpackSource, "Using this checkout");
                return true;
            }
            switch (unpackPayload(*payload, repo,
                                  [&](uint64_t done, uint64_t total) {
                                      set(Stage::UnpackSource, total ? double(done) / double(total) : 0.0,
                                          gigabytes(done) + " of " + gigabytes(total));
                                      return !m_cancel.load();
                                  },
                                  error)) {
            case Unpacked::AlreadyThere: skip(Stage::UnpackSource, "Unpacked before"); return true;
            case Unpacked::Copied: set(Stage::UnpackSource, 1.0, "Version " + payload->version); return true;
            case Unpacked::Failed: break;
            }
            std::lock_guard lock(m_mutex);
            m_error = error;
            return false;
        }) &&
        runStage(Stage::BuildRecompiler, [&] {
            const std::filesystem::path build = repo / "build" / "ps2recomp-standalone";
            const std::string dir = pathUtf8(build);
            return command(Stage::BuildRecompiler,
                           withConfigureArgs(build, {"cmake", "-S", pathUtf8(repo / "thirdparty" / "PS2Recomp"),
                                                     "-B", dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                                                     "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_STUDIO=OFF",
                                                     "-DPS2X_BUILD_TEST=OFF"})) &&
                   command(Stage::BuildRecompiler, {"ninja", "-C", dir, "-j", jobs, "ps2_recomp"});
        }) &&
        runStage(Stage::TranslateGame, [&] {
            // Kept under build/, so deleting build/ also forgets it.
            const std::filesystem::path generated = repo / "build" / "generated";
            const std::filesystem::path stampPath = generated / "launcher-translated.txt";
            const std::string inputs = translationInputs(repo, extracted, hashes);
            std::error_code dirError;
            if (std::filesystem::is_directory(generated / kVersion, dirError) && readText(stampPath) == inputs) {
                skip(Stage::TranslateGame, "Translated before");
                return true;
            }
            std::filesystem::remove(stampPath, dirError);
            std::vector<std::string> args =
                payload ? std::vector<std::string>{pathUtf8(payload->python)} : pythonCommand();
            for (const std::string &arg : {pathUtf8(repo / "setup.py"), std::string("recompile"),
                                           std::string("--version"), std::string(kVersion),
                                           std::string("--extracted"), pathUtf8(extracted),
                                           std::string("--recompiler"), pathUtf8(recompilerPath(repo))})
                args.push_back(arg);
            if (!command(Stage::TranslateGame, args))
                return false;
            // Binary, as readText reads it: Windows text mode would write \r\n.
            std::ofstream(stampPath, std::ios::binary | std::ios::trunc) << inputs;
            size_t files = 0u;
            for (const auto &entry : std::filesystem::directory_iterator(generated / kVersion, dirError))
                files += entry.path().extension() == ".cpp" ? 1u : 0u;
            set(Stage::TranslateGame, 1.0, thousands(files) + " files");
            return true;
        }) &&
        runStage(Stage::ConfigureGame, [&] {
            const std::filesystem::path build = repo / "build" / "game";
            std::vector<std::string> args =
                withConfigureArgs(build, {"cmake", "-S", pathUtf8(repo), "-B", pathUtf8(build), "-G", "Ninja",
                                          "-DCMAKE_BUILD_TYPE=Release", "-DDQ8_LINK_GENERATED=ON",
                                          "-DDQ8_LINK_OVERLAYS=ON", "-DDQ8_GFX_ENABLE_SDLGPU=ON"});
#if defined(__APPLE__)
            // The app bundle the build makes starts the game from these.
            args.push_back("-DDQ8_MACOS_ELF=" + pathUtf8(extracted / kVersion));
            args.push_back("-DDQ8_MACOS_ISO=" + pathUtf8(options.disc));
#elif defined(_WIN32)
            // Without a payload, Windows takes SDL3 and FFmpeg from vcpkg (wiki: Building).
            if (const char *vcpkg = payload ? nullptr : SDL_getenv("VCPKG_ROOT")) {
                const std::filesystem::path toolchain =
                    utf8Path(vcpkg) / "scripts" / "buildsystems" / "vcpkg.cmake";
                std::error_code toolchainError;
                if (std::filesystem::is_regular_file(toolchain, toolchainError))
                    args.push_back("-DCMAKE_TOOLCHAIN_FILE=" + pathUtf8(toolchain));
            }
#endif
            // Without SDL3 the game still builds, then cannot open its window.
            bool sdlgpu = false;
            if (!command(Stage::ConfigureGame, args, nullptr, [&](const std::string &text) {
                    sdlgpu = sdlgpu || text.find("SDL3 GPU backend enabled") != std::string::npos;
                }))
                return false;
            if (!sdlgpu) {
                std::lock_guard lock(m_mutex);
                m_error = "The game's build did not find SDL3 3.2 or newer, which it draws with. Install it "
                          "(see Tools), then build again.";
                return false;
            }
            set(Stage::ConfigureGame, 1.0, "Ready");
            return true;
        }) &&
        // ninja rather than cmake --build: a cancel has to reach ninja, which
        // then stops its compilers.
        runStage(Stage::CompileGame, [&] {
            const std::string dir = pathUtf8(repo / "build" / "game");
            CompilePlan plan;
            plan.scan(repo / "build" / "generated", repo / "build" / "game");
            if (!command(Stage::CompileGame, {"ninja", "-C", dir, "-j", jobs, "dq8"},
                         plan.total > 0.0 ? &plan : nullptr))
                return false;
            markBuilt(repo, {options.workspace, payload ? payload->version : std::string(), options.disc});
            return true;
        });
    m_succeeded = ok;
    m_running = false;
}

} // namespace dq8::launcher
