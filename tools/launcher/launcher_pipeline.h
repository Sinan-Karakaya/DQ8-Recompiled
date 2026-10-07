// From a disc image to a playable game: the steps of the wiki's Building page,
// run one after another on a worker thread, with progress the window draws.
#pragma once

#include "launcher_payload.h"
#include "launcher_process.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dq8::launcher {

enum class Stage : uint8_t {
    CheckDisc, ExtractDisc, UnpackSource, BuildRecompiler, TranslateGame, ConfigureGame, CompileGame, Count
};
constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);
const char *stageTitle(Stage stage);

struct StageState {
    enum class Status : uint8_t { Waiting, Running, Done, Failed, Skipped } status = Status::Waiting;
    double progress = -1.0; // 0 to 1, or negative while unknown
    std::string detail;     // "4,812 of 12,530 files"
    double seconds = 0.0;   // spent, once finished
    double remaining = -1.0; // estimated seconds left, when known
    uint64_t startedMs = 0; // SDL_GetTicks() when it began, for the time so far
};

// What compiling one step costs, in units of a small file (about a second).
// Only the translated functions vary much: time grows with about the cube
// of their size, and 15 of 14k files take ~85% of the CPU time.
double compileCost(const std::string &file, uint64_t bytes);
constexpr double kLargeCompileCost = 100.0;

// "SLUS_212.07/FUN_x.cpp" from a ninja description or an object path under
// build/generated; empty for anything else.
std::string generatedKey(std::string path);

// Every translated file's cost, and which are already compiled. ninja cannot
// list the steps ahead (its dry run stops at the glob check CMake adds), so
// this reads the files: an object newer than its source is compiled.
struct CompilePlan {
    struct File {
        double cost = 1.0;
        bool compiled = false;
    };
    std::unordered_map<std::string, File> files; // by generatedKey
    double total = 0.0;    // every translated file's cost
    double compiled = 0.0; // the cost of those compiled, before or during this build
    size_t pending = 0;    // translated files left to compile
    size_t largeLeft = 0;  // the large ones among them

    void scan(const std::filesystem::path &generated, const std::filesystem::path &buildDir);
    // A finished ninja step: marks the translated file it names compiled, or
    // returns false for any other step.
    bool finish(const std::string &description);
    // A header change recompiles everything, whatever the objects' times.
    void forgetCompiled();
};

struct PipelineOptions {
    std::filesystem::path repo;      // the DQ8Recomp source tree that builds
    std::filesystem::path disc;      // the user's disc image
    std::filesystem::path workspace; // where the disc's files are extracted
    int jobs = 1;                    // parallel compile jobs
    // A release's tools and sources, unpacked into repo first; none from a checkout.
    std::optional<Payload> payload;
};

// What the disc check found, for the window to show before anything is built.
struct DiscInfo {
    bool readable = false;
    std::string error;      // why it cannot be used
    std::string volume;     // ISO volume name
    std::string executable; // "SLUS_212.07"
    uint64_t size = 0;
    bool supported = false; // the revision this project translates
};
DiscInfo inspectDisc(const std::filesystem::path &repo, const std::filesystem::path &disc);

class Pipeline {
public:
    ~Pipeline();
    void start(const PipelineOptions &options, const ChildEnvironment &environment);
    void cancel();
    bool running() const { return m_running.load(); }
    bool succeeded() const { return m_succeeded.load(); }

    // Snapshots for the window, taken under the lock.
    std::array<StageState, kStageCount> stages() const;
    std::vector<std::string> log(size_t lines) const;
    std::string error() const;

private:
    void run(PipelineOptions options, ChildEnvironment environment);
    bool runStage(Stage stage, const std::function<bool()> &body);
    // Runs args for `stage`; with a plan, progress counts each step's cost.
    // `watch` sees every line of output.
    bool command(Stage stage, const std::vector<std::string> &args, CompilePlan *plan = nullptr,
                 const std::function<void(const std::string &)> &watch = {});
    void set(Stage stage, double progress, const std::string &detail);
    // Marks a stage that had nothing to do, with why.
    void skip(Stage stage, const std::string &detail);
    void line(const std::string &text);

    mutable std::mutex m_mutex;
    std::array<StageState, kStageCount> m_stages{};
    std::deque<std::string> m_log;
    std::string m_error;
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_succeeded{false}, m_cancel{false};
    ChildEnvironment m_environment;
    std::filesystem::path m_logFile;
};

// "[1234/12503] Building CXX object ..." gives 1234 and 12503.
bool parseNinjaProgress(const std::string &line, uint64_t &done, uint64_t &total);

// Where the steps put their results inside the source tree.
std::filesystem::path recompilerPath(const std::filesystem::path &repo);
std::filesystem::path gamePath(const std::filesystem::path &repo);
std::filesystem::path extractedDisc(const std::filesystem::path &workspace);

// True when the launcher's last build for this workspace succeeded and what
// Play needs is still there; a stub or a hand-made build does not count.
bool launcherBuilt(const std::filesystem::path &repo, const std::filesystem::path &workspace);

} // namespace dq8::launcher
