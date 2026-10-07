#include "launcher_app.h"

#include "launcher_iso.h"
#include "ui/ui_glyphs.h"
#include "ui/ui_style.h"
#include "ui/ui_widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace dq8::launcher {
using ui::em;
using ui::Icon;
namespace palette = ui::palette;

namespace {
constexpr const char *kPageNames[] = {"Your disc", "Tools", "Options", "Build", "Play"};
constexpr Icon kPageIcons[] = {Icon::Disc, Icon::Wrench, Icon::Interface, Icon::Gear, Icon::Play};
constexpr const char *kVersion = "SLUS_212.07";

// How much of the whole build each step is, for the overall bar.
constexpr double kStageWeight[kStageCount] = {0.04, 0.04, 0.05, 0.08, 0.02, 0.77};

std::string humanSize(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

std::string duration(double seconds) {
    char text[48];
    if (seconds < 60.0)
        std::snprintf(text, sizeof(text), "%.0f s", seconds);
    else if (seconds < 3600.0)
        std::snprintf(text, sizeof(text), "%.0f min", std::ceil(seconds / 60.0));
    else
        std::snprintf(text, sizeof(text), "%d h %02d min", static_cast<int>(seconds / 3600.0),
                      static_cast<int>(std::fmod(seconds, 3600.0) / 60.0));
    return text;
}

ImVec4 color(ImU32 packed) { return ImGui::ColorConvertU32ToFloat4(packed); }

// ui::iconButton's width, to line buttons up at the right.
float buttonWidth(const char *label) {
    return ImGui::GetStyle().FramePadding.x * 2.0f + em(0.95f) + em(0.45f) + ImGui::CalcTextSize(label).x;
}

void title(const char *text, const char *intro) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.55f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    if (intro) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color(palette::kTextMuted));
        ImGui::TextUnformatted(intro);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
}

void wrapped(ImU32 tint, const std::string &text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, color(tint));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

void spinner(ImDrawList *list, ImVec2 center, float radius, double time, ImU32 tint) {
    const float start = static_cast<float>(std::fmod(time * 5.0, 6.2831853));
    list->PathArcTo(center, radius, start, start + 4.2f, 24);
    list->PathStroke(tint, std::max(1.5f, radius * 0.28f));
}

// A rounded bar, gold as it fills; negative fractions sweep while unknown.
void progressBar(double fraction, float height, double time) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList *list = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + width, min.y + height);
    const float r = height * 0.5f;
    list->AddRectFilled(min, max, palette::kFill, r);
    if (fraction < 0.0) {
        const float span = width * 0.28f;
        const float x = min.x + static_cast<float>(std::fmod(time * 0.45, 1.0)) * (width + span) - span;
        list->PushClipRect(min, max, true);
        list->AddRectFilled(ImVec2(x, min.y), ImVec2(x + span, max.y), ui::withAlpha(palette::kGold, 0.75f), r);
        list->PopClipRect();
        return;
    }
    const float fill = std::max(height, width * static_cast<float>(std::clamp(fraction, 0.0, 1.0)));
    list->AddRectFilled(min, ImVec2(min.x + fill, max.y), palette::kGold, r);
    // A highlight that runs along the filled part, so a slow step still moves.
    const float shine = static_cast<float>(std::fmod(time * 0.35, 1.0)) * fill;
    list->PushClipRect(min, ImVec2(min.x + fill, max.y), true);
    list->AddRectFilledMultiColor(ImVec2(min.x + shine - height * 3.0f, min.y), ImVec2(min.x + shine, max.y),
                                  IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 90),
                                  IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 0));
    list->PopClipRect();
}

void stageIcon(ImDrawList *list, ImVec2 center, float size, StageState::Status status, double time) {
    switch (status) {
    case StageState::Status::Done:
        list->AddCircleFilled(center, size * 0.5f, ui::withAlpha(palette::kGood, 0.22f));
        ui::drawIcon(list, Icon::Check, center, size * 0.8f, palette::kGood);
        break;
    case StageState::Status::Skipped:
        list->AddCircleFilled(center, size * 0.5f, palette::kFill);
        ui::drawIcon(list, Icon::Check, center, size * 0.8f, palette::kTextMuted);
        break;
    case StageState::Status::Running:
        spinner(list, center, size * 0.42f, time, palette::kGold);
        break;
    case StageState::Status::Failed:
        list->AddCircleFilled(center, size * 0.5f, ui::withAlpha(palette::kBad, 0.22f));
        ui::drawIcon(list, Icon::Close, center, size * 0.7f, palette::kBad);
        break;
    case StageState::Status::Waiting:
        list->AddCircle(center, size * 0.36f, ui::withAlpha(palette::kTextMuted, 0.6f), 24, 1.5f);
        break;
    }
}

// A read-only line of text in a dark well, with a button that copies it.
void commandWell(const std::string &command, double &copiedAt, double now) {
    const float pad = em(0.6f);
    const float buttonWidth = em(5.5f);
    const float width = ImGui::GetContentRegionAvail().x - buttonWidth - em(0.6f);
    const ImVec2 textSize = ImGui::CalcTextSize(command.c_str(), nullptr, false, width - pad * 2.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + textSize.y + pad * 2.0f);
    ImDrawList *list = ImGui::GetWindowDrawList();
    list->AddRectFilled(min, max, IM_COL32(0, 0, 8, 150), em(0.35f));
    list->AddRect(min, max, palette::kBorderInner, em(0.35f), 1.0f);
    list->AddText(nullptr, 0.0f, ImVec2(min.x + pad, min.y + pad), palette::kText, command.c_str(), nullptr,
                  width - pad * 2.0f);
    ImGui::Dummy(ImVec2(width, max.y - min.y));
    ImGui::SameLine(0.0f, em(0.6f));
    if (ui::iconButton(now - copiedAt < 2.0 ? "Copied" : "Copy", now - copiedAt < 2.0 ? Icon::Check : Icon::Copy)) {
        SDL_SetClipboardText(command.c_str());
        copiedAt = now;
    }
}

void dashedRect(ImDrawList *list, ImVec2 min, ImVec2 max, ImU32 tint, float dash, float phase, float thickness) {
    const auto edge = [&](ImVec2 a, ImVec2 b) {
        const float length = std::hypot(b.x - a.x, b.y - a.y);
        const ImVec2 step((b.x - a.x) / length, (b.y - a.y) / length);
        for (float at = std::fmod(phase, dash * 2.0f) - dash * 2.0f; at < length; at += dash * 2.0f) {
            const float from = std::max(0.0f, at), to = std::min(length, at + dash);
            if (to > from)
                list->AddLine(ImVec2(a.x + step.x * from, a.y + step.y * from),
                              ImVec2(a.x + step.x * to, a.y + step.y * to), tint, thickness);
        }
    };
    edge(min, ImVec2(max.x, min.y));
    edge(ImVec2(max.x, min.y), max);
    edge(max, ImVec2(min.x, max.y));
    edge(ImVec2(min.x, max.y), min);
}

constexpr SDL_DialogFileFilter kDiscFilters[] = {{"Disc images", "iso"}, {"All files", "*"}};

// SDL's picker callbacks; userdata is a share of the app's DialogPicks, made
// for this one answer.
void SDLCALL onDiscPicked(void *userdata, const char *const *files, int) {
    const std::unique_ptr<std::shared_ptr<DialogPicks>> picks(static_cast<std::shared_ptr<DialogPicks> *>(userdata));
    if (!files || !files[0])
        return;
    std::lock_guard lock((*picks)->mutex);
    (*picks)->disc = files[0];
}

void SDLCALL onWorkspacePicked(void *userdata, const char *const *folders, int) {
    const std::unique_ptr<std::shared_ptr<DialogPicks>> picks(static_cast<std::shared_ptr<DialogPicks> *>(userdata));
    if (!folders || !folders[0])
        return;
    std::lock_guard lock((*picks)->mutex);
    (*picks)->workspace = folders[0];
}

std::string iniEscape(const std::string &text) {
    std::string out;
    for (const char c : text)
        if (c != '\n' && c != '\r')
            out += c;
    return out;
}
} // namespace

const char *previewName(Preview preview) {
    switch (preview) {
    case Preview::DiscEmpty: return "disc-empty";
    case Preview::DiscReady: return "disc-ready";
    case Preview::ToolsReady: return "tools-ready";
    case Preview::ToolsMissing: return "tools-missing";
    case Preview::Options: return "options";
    case Preview::Building: return "building";
    case Preview::BuildFailed: return "build-failed";
    case Preview::Play: return "play";
    case Preview::Count: break;
    }
    return "";
}

std::filesystem::path configPath() {
    char *folder = SDL_GetPrefPath("DQ8Recomp", "Launcher");
    if (!folder)
        return "dq8-launcher.ini";
    std::filesystem::path path = utf8Path(folder) / "launcher.ini";
    SDL_free(folder);
    return path;
}

LauncherConfig loadConfig() {
    LauncherConfig config;
    std::ifstream file(configPath());
    std::string line;
    while (std::getline(file, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1u);
        if (key == "disc")
            config.disc = value;
        else if (key == "workspace")
            config.workspace = value;
        else if (key == "jobs")
            config.jobs = std::atoi(value.c_str());
    }
    return config;
}

void saveConfig(const LauncherConfig &config) {
    std::ofstream file(configPath(), std::ios::trunc);
    file << "disc=" << iniEscape(config.disc) << "\nworkspace=" << iniEscape(config.workspace)
         << "\njobs=" << config.jobs << '\n';
}

int defaultJobs() {
    const int cores = std::max(1, SDL_GetNumLogicalCPUCores());
    // Clang peaks at 1.5 GB on the largest translated function (measured over
    // a full build), so allow 2 GB a job.
    const int memory = std::max(1, SDL_GetSystemRAM() / 2048);
    return std::clamp(std::min(cores, memory), 1, cores);
}

LauncherApp::LauncherApp(std::filesystem::path repo, SDL_Window *window, const LauncherConfig &overrides,
                         bool persist)
    : m_repo(std::move(repo)), m_window(window), m_config(loadConfig()), m_persist(persist) {
    if (!overrides.disc.empty())
        m_config.disc = overrides.disc;
    if (!overrides.workspace.empty())
        m_config.workspace = overrides.workspace;
    if (overrides.jobs > 0)
        m_config.jobs = overrides.jobs;
    if (m_config.workspace.empty())
        m_config.workspace = pathUtf8(m_repo.parent_path());
    if (m_config.jobs <= 0)
        m_config.jobs = defaultJobs();
    m_gameSettingsPath = ui::defaultSettingsPath();
    if (m_persist)
        ui::loadSettings(m_gameSettingsPath, m_gameSettings);
    std::error_code ec;
    if (!m_config.disc.empty() && std::filesystem::is_regular_file(utf8Path(m_config.disc), ec))
        setDisc(m_config.disc);
    checkToolsAsync();
    if (m_disc && m_disc->supported)
        m_page = launcherBuilt(m_repo, utf8Path(m_config.workspace)) ? Page::Play : Page::Tools;
}

LauncherApp::~LauncherApp() {
    m_pipeline.cancel();
    if (m_toolsThread.joinable())
        m_toolsThread.join();
}

void LauncherApp::handleEvent(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_DROP_BEGIN:
        m_dragging = true;
        break;
    case SDL_EVENT_DROP_COMPLETE:
        m_dragging = false;
        break;
    case SDL_EVENT_DROP_FILE:
        m_dragging = false;
        if (event.drop.data && !m_pipeline.running()) {
            setDisc(event.drop.data);
            m_page = Page::Disc;
        }
        break;
    default:
        break;
    }
}

void LauncherApp::setDisc(const std::string &path) {
    m_disc = inspectDisc(m_repo, utf8Path(path));
    m_config.disc = path;
    if (m_live && m_persist)
        saveConfig(m_config);
}

void LauncherApp::checkToolsAsync() {
    if (m_checkingTools.exchange(true))
        return;
    if (m_toolsThread.joinable())
        m_toolsThread.join();
    m_toolsThread = std::thread([this] {
        ToolReport report = checkTools(ChildEnvironment{extraToolDirs(), {}});
        // The source tree's own parts, which a download without submodules lacks.
        for (const char *part : {"thirdparty/PS2Recomp/CMakeLists.txt", "thirdparty/imgui/imgui.cpp",
                                 "thirdparty/simde/simde"}) {
            std::error_code ec;
            if (!std::filesystem::exists(m_repo / part, ec)) {
                ToolCheck tree{"Source tree", "DQ8Recomp's own parts"};
                tree.found = true;
                tree.version = "incomplete";
                tree.problem = std::string("Missing ") + part + ": download the full release, or run "
                               "git submodule update --init --recursive.";
                report.tools.push_back(tree);
                break;
            }
        }
        std::lock_guard lock(m_toolsMutex);
        m_tools = std::move(report);
        m_checkingTools = false;
    });
}

bool LauncherApp::pageDone(Page page) const {
    switch (page) {
    case Page::Disc: return m_disc && m_disc->supported;
    case Page::Tools: {
        std::lock_guard lock(m_toolsMutex);
        return m_tools && m_tools->ready();
    }
    case Page::Options: return true;
    case Page::Build: {
        if (!m_live)
            return m_page == Page::Play;
        if (m_pipeline.running())
            return false;
        // Asked several times a frame; the files behind it change rarely.
        if (m_time - m_builtCheckedAt > 0.5 || m_time < m_builtCheckedAt) {
            m_built = launcherBuilt(m_repo, utf8Path(m_config.workspace));
            m_builtCheckedAt = m_time;
        }
        return m_built;
    }
    case Page::Play:
    case Page::Count: break;
    }
    return false;
}

bool LauncherApp::pageReachable(Page page) const {
    switch (page) {
    case Page::Disc: return true;
    case Page::Tools: return pageDone(Page::Disc);
    case Page::Options:
    case Page::Build: return pageDone(Page::Disc) && pageDone(Page::Tools);
    case Page::Play: return pageDone(Page::Build);
    case Page::Count: break;
    }
    return false;
}

void LauncherApp::goTo(Page page) {
    if (pageReachable(page))
        m_page = page;
}

void LauncherApp::writeGameSettings() {
    if (m_live && m_persist)
        ui::saveSettings(m_gameSettingsPath, m_gameSettings);
}

void LauncherApp::startBuild() {
    if (!m_live || !m_persist || m_pipeline.running() || !m_disc)
        return;
    saveConfig(m_config);
    writeGameSettings();
    PipelineOptions options;
    options.repo = m_repo;
    options.disc = utf8Path(m_config.disc);
    options.workspace = utf8Path(m_config.workspace);
    options.jobs = m_config.jobs;
    m_pipeline.start(options, ChildEnvironment{extraToolDirs(), {}});
}

void LauncherApp::launchGame() {
    m_launchError.clear();
    if (!m_live)
        return;
    writeGameSettings();
    const std::filesystem::path workspace = utf8Path(m_config.workspace);
    if (!launcherBuilt(m_repo, workspace)) {
        m_launchError = "The game or its files are missing from " + pathUtf8(extractedDisc(workspace)) +
                        ". Build again.";
        return;
    }
    const std::vector<std::string> args = {pathUtf8(gamePath(m_repo)),
                                           pathUtf8(extractedDisc(workspace) / kVersion),
                                           "--iso=" + m_config.disc, "--gs=sdlgpu"};
    std::vector<const char *> argv;
    for (const std::string &arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    // The game's own log, for reports.
    SDL_IOStream *log = SDL_IOFromFile(pathUtf8(workspace / "dq8.log").c_str(), "w");
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    if (log) {
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    }
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (log)
        SDL_CloseIO(log);
    if (!process) {
        m_launchError = std::string("The game did not start: ") + SDL_GetError();
        return;
    }
    // A background process keeps running when its handle goes.
    SDL_DestroyProcess(process);
}

void LauncherApp::showPreview(Preview preview) {
    m_live = false;
    // The real check started with the window; it must not land over the fixed report.
    if (m_toolsThread.joinable())
        m_toolsThread.join();
    m_checkingTools = false;
    // Never the player's own settings in a screenshot.
    m_gameSettings = ui::Settings{};
    m_config.disc = "/Users/you/Games/Dragon Quest VIII (USA).iso";
    m_config.workspace = "/Users/you/Games";
    m_config.jobs = 8;
    DiscInfo disc;
    disc.readable = true;
    disc.volume = "SLUS_21207";
    disc.executable = kVersion;
    disc.size = 4180148224ull;
    disc.supported = true;
    m_disc = disc;
    ToolReport tools;
    const auto add = [&](const char *name, const char *purpose, const char *version, bool optional = false) {
        ToolCheck check{name, purpose};
        check.found = version != nullptr;
        check.version = version ? version : "";
        check.optional = optional;
        tools.tools.push_back(check);
    };
    const bool missing = preview == Preview::ToolsMissing;
    add("CMake", "Configures the build", missing ? nullptr : "3.31.6");
    add("Ninja", "Runs the build", missing ? nullptr : "1.12.1");
    add("Python", "Translates the game code", "3.13.1");
    add("C++ compiler", "Compiles the game", "17.0.0");
    add("pkg-config", "Finds SDL3 and FFmpeg", "2.5.1");
    add("SDL3", "Window, graphics, sound and controllers", missing ? nullptr : "3.4.16");
    add("FFmpeg", "Plays the movies; without it they are skipped", "62.11.100", true);
    add("LLVM", "Archives the large compiled game", "21.1.0");
    if (missing) {
        tools.installCommand = "xcode-select --install; brew install cmake ninja pkgconf sdl3 ffmpeg llvm python";
        tools.instructions = "Paste this into Terminal, wait for it to finish, then check again.";
    }
    {
        std::lock_guard lock(m_toolsMutex);
        m_tools = tools;
    }
    m_previewStages = {};
    m_previewError.clear();
    m_previewLog.clear();
    const auto done = [&](Stage stage, double seconds, const char *detail) {
        StageState &state = m_previewStages[static_cast<size_t>(stage)];
        state.status = StageState::Status::Done;
        state.progress = 1.0;
        state.seconds = seconds;
        state.detail = detail;
    };
    switch (preview) {
    case Preview::DiscEmpty:
        m_disc.reset();
        m_page = Page::Disc;
        break;
    case Preview::DiscReady: m_page = Page::Disc; break;
    case Preview::ToolsReady:
    case Preview::ToolsMissing: m_page = Page::Tools; break;
    case Preview::Options: m_page = Page::Options; break;
    case Preview::Building:
    case Preview::BuildFailed: {
        m_page = Page::Build;
        // A real first build on an M-series Mac, 25 minutes into compiling.
        done(Stage::CheckDisc, 23.0, "4.18 GB checked");
        done(Stage::ExtractDisc, 4.0, "4.18 GB copied");
        done(Stage::BuildRecompiler, 58.0, "123 of 123");
        done(Stage::TranslateGame, 6.0, "12,446 files");
        done(Stage::ConfigureGame, 34.0, "Ready");
        StageState &compile = m_previewStages[static_cast<size_t>(Stage::CompileGame)];
        compile.status = preview == Preview::Building ? StageState::Status::Running : StageState::Status::Failed;
        compile.progress = 0.14;
        compile.detail = "10,972 of 14,253  15 large files left";
        compile.seconds = 1500.0;
        compile.remaining = -1.0;
        const std::string object = "Building CXX object src/runtime/CMakeFiles/dq8_generated.dir/__/__/build/generated/"
                                   "SLUS_212.07/";
        m_previewLog = {"[10969/14253] " + object + "FUN_00378db0_0x378db0.cpp.o",
                        "[10970/14253] " + object + "FUN_00378e90_0x378e90.cpp.o",
                        "[10971/14253] " + object + "FUN_00379090_0x379090.cpp.o",
                        "[10972/14253] " + object + "FUN_003791d0_0x3791d0.cpp.o"};
        if (preview == Preview::BuildFailed) {
            m_previewLog.push_back("clang++: error: unable to execute command: Killed");
            m_previewLog.push_back("ninja: build stopped: subcommand failed.");
            m_previewError = "Compile the game failed; the log shows why. Running out of memory is the usual cause: "
                             "lower the compile jobs in Options and try again.";
        }
        break;
    }
    case Preview::Play:
        m_page = Page::Play;
        for (size_t i = 0; i < kStageCount; ++i)
            m_previewStages[i].status = StageState::Status::Done;
        break;
    case Preview::Count: break;
    }
}

void LauncherApp::drawBackground() {
    ImDrawList *list = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    list->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(10, 14, 44, 255), IM_COL32(18, 26, 74, 255),
                                  IM_COL32(32, 22, 58, 255), IM_COL32(8, 10, 30, 255));
    // A few slow stars, as the game's own menus sit over a night sky.
    for (int i = 0; i < 70; ++i) {
        const float x = std::fmod(static_cast<float>(i) * 197.31f, 1.0f * size.x);
        const float y = std::fmod(static_cast<float>(i * i) * 61.7f + static_cast<float>(i) * 13.0f, size.y * 0.75f);
        const float twinkle = 0.45f + 0.55f * std::sin(static_cast<float>(m_time) * (0.6f + (i % 7) * 0.13f) + i);
        list->AddCircleFilled(ImVec2(x, y), 0.6f + (i % 3) * 0.5f,
                              IM_COL32(255, 255, 255, static_cast<int>(40 + 90 * twinkle * twinkle)));
    }
    list->AddRectFilledMultiColor(ImVec2(0, size.y * 0.72f), size, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                  IM_COL32(244, 203, 82, 26), IM_COL32(244, 203, 82, 26));
}

void LauncherApp::draw() {
    m_time = ImGui::GetTime();
    std::optional<std::string> pickedDisc, pickedWorkspace;
    {
        std::lock_guard lock(m_picks->mutex);
        pickedDisc = std::exchange(m_picks->disc, std::nullopt);
        pickedWorkspace = std::exchange(m_picks->workspace, std::nullopt);
    }
    if (pickedDisc)
        setDisc(*pickedDisc);
    if (pickedWorkspace) {
        m_config.workspace = *pickedWorkspace;
        if (m_live && m_persist)
            saveConfig(m_config);
    }
    // A finished build moves on to Play by itself.
    if (m_live && m_page == Page::Build && m_pipeline.succeeded() && !m_pipeline.running())
        m_page = Page::Play;

    drawBackground();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = em(1.4f);
    const ImVec2 min(margin, margin), max(display.x - margin, display.y - margin);
    ImDrawList *back = ImGui::GetBackgroundDrawList();
    ui::drawShadow(back, min, max, em(0.8f));
    back->AddRectFilled(min, max, palette::kWindow, em(0.8f));
    ui::drawFrame(back, min, max, em(0.8f));

    ImGui::SetNextWindowPos(ImVec2(min.x + em(1.2f), min.y + em(1.0f)));
    ImGui::SetNextWindowSize(ImVec2(max.x - min.x - em(2.4f), max.y - min.y - em(2.0f)));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Header: the crown, the name and what it is for.
    ui::icon(Icon::Crown, em(2.2f), palette::kGold);
    ImGui::SameLine(0.0f, em(0.7f));
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::TextUnformatted("DQ8Recomp");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, color(palette::kTextMuted));
    ImGui::TextUnformatted("Play Dragon Quest VIII natively, from your own disc");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, em(0.4f)));

    const float sidebar = em(13.5f);
    const float bodyHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##sidebar", ImVec2(sidebar, bodyHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    drawSidebar(sidebar);
    ImGui::EndChild();
    ImGui::SameLine(0.0f, em(1.2f));
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x - em(0.6f), at.y), ImVec2(at.x - em(0.6f), at.y + bodyHeight),
                                            palette::kBorderInner, 1.0f);
    }
    ImGui::BeginChild("##page", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::BeginChild("##page-body", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - em(3.2f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    switch (m_page) {
    case Page::Disc: drawDiscPage(); break;
    case Page::Tools: drawToolsPage(); break;
    case Page::Options: drawOptionsPage(); break;
    case Page::Build: drawBuildPage(); break;
    case Page::Play: drawPlayPage(); break;
    case Page::Count: break;
    }
    ImGui::EndChild();
    drawFooter();
    ImGui::EndChild();
    ImGui::End();
}

void LauncherApp::drawSidebar(float width) {
    (void)width;
    for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
        const auto page = static_cast<Page>(index);
        const bool reachable = pageReachable(page);
        ImGui::BeginDisabled(!reachable);
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        if (ui::navItem(kPageNames[index], kPageIcons[index], m_page == page))
            goTo(page);
        ImGui::EndDisabled();
        if (reachable && pageDone(page) && page != Page::Play && page != Page::Options) {
            const float h = em(2.3f);
            ui::drawIcon(ImGui::GetWindowDrawList(), Icon::Check, ImVec2(top.x + rowWidth - em(1.1f), top.y + h * 0.5f),
                         em(1.0f), palette::kGood);
        }
    }
    ImGui::Dummy(ImVec2(0.0f, em(1.0f)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, color(palette::kTextMuted));
    ImGui::TextUnformatted("Your disc and your saves stay on this computer. Nothing from the game is "
                           "downloaded or shared.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

void LauncherApp::drawFooter() {
    ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
    const ImVec2 row = ImGui::GetCursorPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const int index = static_cast<int>(m_page);
    if (index > 0 && !(m_live && m_pipeline.running())) {
        if (ui::iconButton("Back", Icon::Reset))
            m_page = static_cast<Page>(index - 1);
    }

    // The page's own actions, at the right end, always in view.
    struct Action {
        const char *label;
        Icon icon;
        bool primary, enabled;
        int id;
    };
    std::vector<Action> actions;
    if (m_page == Page::Build) {
        const auto stages = m_live ? m_pipeline.stages() : m_previewStages;
        const bool running = std::any_of(stages.begin(), stages.end(), [](const StageState &stage) {
            return stage.status == StageState::Status::Running;
        });
        const bool started = std::any_of(stages.begin(), stages.end(), [](const StageState &stage) {
            return stage.status != StageState::Status::Waiting;
        });
        actions.push_back({m_showLog ? "Hide details" : "Show details", Icon::Chart, false, true, 0});
        if (running)
            actions.push_back({"Cancel", Icon::Close, false, true, 1});
        else
            actions.push_back({started ? "Build again" : "Start building", Icon::Gear, true, true, 2});
    } else if (m_page != Page::Play) {
        const auto next = static_cast<Page>(index + 1);
        actions.push_back({"Continue", Icon::Play, true, pageDone(m_page) && pageReachable(next), 3});
    }
    float total = 0.0f;
    for (const Action &action : actions)
        total += buttonWidth(action.label) + em(0.6f);
    ImGui::SetCursorPos(ImVec2(row.x + width - total + em(0.6f), row.y));
    for (const Action &action : actions) {
        ImGui::BeginDisabled(!action.enabled);
        const bool pressed = ui::iconButton(action.label, action.icon, action.primary);
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, em(0.6f));
        if (!pressed)
            continue;
        switch (action.id) {
        case 0: m_showLog = !m_showLog; break;
        case 1:
            if (m_live)
                m_pipeline.cancel();
            break;
        case 2: startBuild(); break;
        case 3: goTo(static_cast<Page>(index + 1)); break;
        default: break;
        }
    }
    ImGui::NewLine();
}

void LauncherApp::drawDiscPage() {
    title("Your disc",
          "DQ8Recomp turns your own copy of Dragon Quest VIII into a game that runs natively on this "
          "computer. It needs a disc image (.iso) of the North American release, SLUS-21207. Nothing from "
          "the game comes with DQ8Recomp.");
    ImDrawList *list = ImGui::GetWindowDrawList();
    const bool ready = m_disc && m_disc->supported;
    if (!ready) {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = em(12.5f);
        const ImVec2 max(min.x + width, min.y + height);
        const bool hot = m_dragging || ImGui::IsMouseHoveringRect(min, max);
        list->AddRectFilled(min, max, hot ? ui::withAlpha(palette::kGold, 0.10f) : palette::kFill, em(0.8f));
        dashedRect(list, ImVec2(min.x + 2, min.y + 2), ImVec2(max.x - 2, max.y - 2),
                   hot ? palette::kGold : ui::withAlpha(palette::kTextMuted, 0.7f), em(0.6f),
                   static_cast<float>(m_time) * (hot ? 24.0f : 8.0f), 2.0f);
        const ImVec2 center(min.x + width * 0.5f, min.y + em(3.8f));
        ui::drawIcon(list, Icon::Disc, center, em(4.0f), hot ? palette::kGold : palette::kText);
        const char *line = m_dragging ? "Drop it here" : "Drop your disc image here";
        // Rows in the page's own ems, before the larger font changes what em() means.
        const float lineY = min.y + em(6.3f), alternativeY = min.y + em(8.0f), buttonY = min.y + em(9.6f);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        const ImVec2 lineSize = ImGui::CalcTextSize(line);
        list->AddText(ImVec2(center.x - lineSize.x * 0.5f, lineY), palette::kText, line);
        ImGui::PopFont();
        const char *alternative = "or choose it from a folder";
        const ImVec2 alternativeSize = ImGui::CalcTextSize(alternative);
        list->AddText(ImVec2(center.x - alternativeSize.x * 0.5f, alternativeY), palette::kTextMuted, alternative);
        ImGui::SetCursorScreenPos(ImVec2(center.x - buttonWidth("Browse...") * 0.5f, buttonY));
        if (ui::iconButton("Browse...", Icon::Folder, !m_dragging)) {
            SDL_ShowOpenFileDialog(onDiscPicked, new std::shared_ptr<DialogPicks>(m_picks), m_window, kDiscFilters, 2,
                                   nullptr, false);
        }
        ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + em(0.8f)));
        // An item, so the zone counts toward the page's size.
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        if (m_disc && !m_disc->error.empty()) {
            ui::icon(Icon::Warning, em(1.2f), palette::kBad);
            ImGui::SameLine(0.0f, em(0.5f));
            wrapped(palette::kBad, m_disc->error);
        }
        return;
    }

    // The recognised disc, as a card.
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = em(9.6f);
    const ImVec2 max(min.x + width, min.y + height);
    list->AddRectFilled(min, max, palette::kFillStrong, em(0.8f));
    list->AddRect(min, max, ui::withAlpha(palette::kGold, 0.85f), em(0.8f), 1.5f);
    ui::drawIcon(list, Icon::Disc, ImVec2(min.x + em(3.2f), min.y + height * 0.5f), em(4.2f), palette::kGold);
    ImGui::SetCursorScreenPos(ImVec2(min.x + em(6.4f), min.y + em(1.0f)));
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted("Dragon Quest VIII: Journey of the Cursed King");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, color(palette::kTextMuted));
    ImGui::Text("North American release  %s  %s", m_disc->executable.c_str(), humanSize(m_disc->size).c_str());
    ImGui::PushTextWrapPos(max.x - em(1.0f));
    ImGui::TextUnformatted(m_config.disc.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
    ui::icon(Icon::Check, em(1.1f), palette::kGood);
    ImGui::SameLine(0.0f, em(0.4f));
    ImGui::TextColored(color(palette::kGood), "Recognised. Every byte is checked when the build starts.");
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + em(1.0f)));
    if (ui::iconButton("Choose another disc...", Icon::Folder))
        SDL_ShowOpenFileDialog(onDiscPicked, new std::shared_ptr<DialogPicks>(m_picks), m_window, kDiscFilters, 2,
                               nullptr, false);
}

void LauncherApp::drawToolsPage() {
    title("Tools",
          "Building the game takes a few free developer tools. The launcher looks for them; it installs "
          "nothing by itself.");
    std::optional<ToolReport> report;
    {
        std::lock_guard lock(m_toolsMutex);
        report = m_tools;
    }
    ImDrawList *list = ImGui::GetWindowDrawList();
    if (!report || m_checkingTools.load()) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        spinner(list, ImVec2(at.x + em(0.7f), at.y + em(0.7f)), em(0.55f), m_time, palette::kGold);
        ImGui::Dummy(ImVec2(em(1.6f), em(1.4f)));
        ImGui::SameLine();
        ImGui::TextUnformatted("Looking for the tools...");
        if (!report)
            return;
        ImGui::Dummy(ImVec2(0.0f, em(0.4f)));
    }
    // The verdict comes first, with what to do when something is missing; the
    // list follows.
    if (report->ready()) {
        ui::icon(Icon::Check, em(1.1f), palette::kGood);
        ImGui::SameLine(0.0f, em(0.4f));
        ImGui::TextColored(color(palette::kGood), "Everything the build needs is here.");
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
    } else {
        if (!report->instructions.empty())
            wrapped(palette::kText, report->instructions);
        if (!report->installCommand.empty()) {
            ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
            commandWell(report->installCommand, m_copiedAt, m_time);
        }
        ImGui::BeginDisabled(m_checkingTools.load() || !m_live);
        if (ui::iconButton("Check again", Icon::Reset))
            checkToolsAsync();
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
    }
    if (ImGui::BeginTable("##tools", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed, em(2.0f));
        ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##version", ImGuiTableColumnFlags_WidthFixed, em(9.0f));
        const float row = em(1.9f);
        const float padding = ImGui::GetStyle().CellPadding.y;
        for (const ToolCheck &tool : report->tools) {
            // One line a tool, so the list fits without scrolling; a problem
            // gets a second, wrapped line.
            const bool oneLine = tool.problem.empty();
            const float lift = oneLine ? std::max(0.0f, (row - ImGui::GetTextLineHeight()) * 0.5f - padding) : 0.0f;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row);
            ImGui::TableNextColumn();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 center(at.x + em(0.9f), oneLine ? at.y - padding + row * 0.5f : at.y + em(1.05f));
            if (tool.usable())
                stageIcon(list, center, em(1.2f), StageState::Status::Done, m_time);
            else if (tool.optional)
                ui::drawIcon(list, Icon::Warning, center, em(1.1f), palette::kWarn);
            else
                stageIcon(list, center, em(1.2f), StageState::Status::Failed, m_time);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
            ImGui::TextUnformatted(tool.name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, color(palette::kTextMuted));
            if (oneLine) {
                ImGui::SameLine(0.0f, em(0.7f));
                ImGui::TextUnformatted(tool.purpose.c_str());
            } else {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(tool.problem.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
            if (tool.found)
                ImGui::TextColored(color(tool.problem.empty() ? palette::kText : palette::kWarn), "%s",
                                   tool.version.c_str());
            else
                ImGui::TextColored(color(tool.optional ? palette::kWarn : palette::kBad), "Not found");
        }
        ImGui::EndTable();
    }
}

void LauncherApp::drawOptionsPage() {
    title("Options", "You can change these later, too: press F1 in the game for its menu.");
    ui::sectionHeader("Files");
    if (ui::beginSettings("files")) {
        ui::settingRow("Game files", "The disc's files are copied here; your saves live with them.");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(pathUtf8(extractedDisc(utf8Path(m_config.workspace))).c_str());
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(m_pipeline.running() || !m_live);
        if (ui::iconButton("Change...", Icon::Folder))
            SDL_ShowOpenFolderDialog(onWorkspacePicked, new std::shared_ptr<DialogPicks>(m_picks), m_window,
                                     m_config.workspace.c_str(), false);
        ImGui::EndDisabled();
        ui::endSettings();
    }
    ui::sectionHeader("Build");
    if (ui::beginSettings("build")) {
        ui::settingRow("Compile jobs", "More build faster but take more memory. The first build takes an hour or "
                                       "more, mostly a few very large files; later ones only redo what changed.");
        const int cores = std::max(1, SDL_GetNumLogicalCPUCores());
        int jobs = std::clamp(m_config.jobs, 1, cores);
        if (ImGui::SliderInt("##jobs", &jobs, 1, cores)) {
            m_config.jobs = jobs;
            if (m_live && m_persist)
                saveConfig(m_config);
        }
        ui::endSettings();
    }
    ui::sectionHeader("Picture");
    if (ui::beginSettings("picture")) {
        bool changed = false;
        ui::settingRow("Internal resolution", "A multiple of the PS2's own resolution.");
        static const char *const scales[] = {"1x", "2x", "3x", "4x"};
        int scale = std::clamp(static_cast<int>(m_gameSettings.resolutionScale), 1, 4) - 1;
        if (ui::segmented("scale", &scale, scales, 4)) {
            m_gameSettings.resolutionScale = static_cast<uint32_t>(scale + 1);
            changed = true;
        }
        ui::settingRow("Window");
        static const char *const modes[] = {"Windowed", "Fullscreen"};
        int mode = m_gameSettings.fullscreen ? 1 : 0;
        if (ui::segmented("mode", &mode, modes, 2)) {
            m_gameSettings.fullscreen = mode == 1;
            changed = true;
        }
        ui::settingRow("Aspect ratio", "Auto follows the game's own Screen Size option.");
        static const char *const aspects[] = {"Auto", "4:3", "16:9"};
        int aspect = std::min(static_cast<int>(m_gameSettings.display.aspect), 2);
        if (ui::segmented("aspect", &aspect, aspects, 3)) {
            m_gameSettings.display.aspect = static_cast<gfx::SdlGpuAspect>(aspect);
            changed = true;
        }
        if (changed)
            writeGameSettings();
        ui::endSettings();
    }
}

void LauncherApp::drawBuildPage() {
    title("Build", "Everything happens on this computer. You can keep using it meanwhile; closing the launcher "
                   "stops the build.");
    const auto stages = m_live ? m_pipeline.stages() : m_previewStages;
    const bool running = m_live ? m_pipeline.running()
                                : std::any_of(stages.begin(), stages.end(), [](const StageState &stage) {
                                      return stage.status == StageState::Status::Running;
                                  });
    const std::string error = m_live ? m_pipeline.error() : m_previewError;

    // A running step's time so far; previews carry it in `seconds`.
    const auto elapsed = [](const StageState &stage) {
        if (stage.status != StageState::Status::Running || stage.startedMs == 0u)
            return stage.seconds;
        return static_cast<double>(SDL_GetTicks() - stage.startedMs) / 1000.0;
    };
    // The whole build, weighted by how long each step usually takes.
    double overall = 0.0, remaining = -1.0, spent = 0.0;
    for (size_t i = 0; i < kStageCount; ++i) {
        const StageState &stage = stages[i];
        if (stage.status == StageState::Status::Done || stage.status == StageState::Status::Skipped)
            overall += kStageWeight[i];
        else if (stage.status == StageState::Status::Running && stage.progress > 0.0)
            overall += kStageWeight[i] * stage.progress;
        if (stage.status == StageState::Status::Running && stage.remaining >= 0.0)
            remaining = stage.remaining;
        spent += elapsed(stage);
    }
    const bool started = std::any_of(stages.begin(), stages.end(), [](const StageState &stage) {
        return stage.status != StageState::Status::Waiting;
    });
    if (started) {
        char line[96];
        std::snprintf(line, sizeof(line), "%.0f%%", overall * 100.0);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
        ImGui::TextUnformatted(line);
        ImGui::PopFont();
        if (running && (remaining >= 0.0 || spent >= 60.0)) {
            ImGui::SameLine(0.0f, em(0.8f));
            ImGui::AlignTextToFramePadding();
            // No guess while the large files are compiling: a count of files
            // says nothing about them.
            if (remaining >= 0.0)
                ImGui::TextColored(color(palette::kTextMuted), "about %s left", duration(remaining).c_str());
            else
                ImGui::TextColored(color(palette::kTextMuted), "%s so far", duration(spent).c_str());
        }
        progressBar(overall, em(0.7f), m_time);
        ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
    }

    // One line a step: its state, its name, and what it is doing or took.
    ImDrawList *list = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < kStageCount; ++i) {
        const StageState &stage = stages[i];
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float right = at.x + ImGui::GetContentRegionAvail().x;
        const float line = ImGui::GetTextLineHeight();
        stageIcon(list, ImVec2(at.x + em(0.65f), at.y + line * 0.5f), em(1.15f), stage.status, m_time);
        ImGui::SetCursorScreenPos(ImVec2(at.x + em(1.8f), at.y));
        const ImU32 tint = stage.status == StageState::Status::Waiting ? palette::kTextMuted : palette::kText;
        ImGui::TextColored(color(tint), "%s", stageTitle(static_cast<Stage>(i)));
        std::string detail = stage.detail;
        if (stage.status == StageState::Status::Running && stage.remaining >= 0.0)
            detail += (detail.empty() ? "" : "  ") + std::string("about ") + duration(stage.remaining) + " left";
        else if (stage.status == StageState::Status::Running && elapsed(stage) >= 60.0)
            detail += (detail.empty() ? "" : "  ") + duration(elapsed(stage)) + " so far";
        else if (stage.status != StageState::Status::Running && stage.seconds >= 1.0)
            detail += (detail.empty() ? "" : "  ") + duration(stage.seconds);
        if (!detail.empty()) {
            const float detailWidth = ImGui::CalcTextSize(detail.c_str()).x;
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, right - detailWidth), at.y));
            ImGui::TextColored(color(palette::kTextMuted), "%s", detail.c_str());
        }
        if (stage.status == StageState::Status::Running) {
            ImGui::SetCursorScreenPos(ImVec2(at.x + em(1.8f), ImGui::GetCursorScreenPos().y + em(0.1f)));
            progressBar(stage.progress, em(0.32f), m_time);
        }
        ImGui::Dummy(ImVec2(0.0f, em(0.25f)));
    }

    if (!error.empty()) {
        ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
        ui::icon(Icon::Warning, em(1.2f), palette::kBad);
        ImGui::SameLine(0.0f, em(0.5f));
        wrapped(palette::kBad, error);
    }
    (void)started;
    if (m_showLog || (!m_live && !m_previewLog.empty())) {
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        const std::vector<std::string> lines = m_live ? m_pipeline.log(300) : m_previewLog;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, color(IM_COL32(0, 0, 8, 150)));
        ImGui::BeginChild("##log", ImVec2(0.0f, std::max(em(5.0f), ImGui::GetContentRegionAvail().y - em(0.2f))),
                          ImGuiChildFlags_Borders);
        ImGui::PushStyleColor(ImGuiCol_Text, color(palette::kTextMuted));
        // Wrapped: the file a line names is at its end.
        ImGui::PushTextWrapPos(0.0f);
        for (const std::string &line : lines)
            ImGui::TextUnformatted(line.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - em(2.0f))
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void LauncherApp::drawPlayPage() {
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0.0f, em(1.5f)));
    const ImVec2 crownAt = ImGui::GetCursorScreenPos();
    ui::drawIcon(ImGui::GetWindowDrawList(), Icon::Crown, ImVec2(crownAt.x + width * 0.5f, crownAt.y + em(2.2f)),
                 em(4.4f), palette::kGold);
    ImGui::Dummy(ImVec2(0.0f, em(4.6f)));
    const auto centered = [&](const char *text, ImU32 tint, float scale) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
        const float w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - w) * 0.5f));
        ImGui::TextColored(color(tint), "%s", text);
        ImGui::PopFont();
    };
    centered("Ready to play", palette::kText, 1.9f);
    centered("Built on this computer from your disc.", palette::kTextMuted, 1.0f);
    ImGui::Dummy(ImVec2(0.0f, em(1.4f)));

    // The big button.
    const float buttonWidth = em(14.0f), buttonHeight = em(3.4f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - buttonWidth) * 0.5f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##play", ImVec2(buttonWidth, buttonHeight), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList *list = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + buttonWidth, min.y + buttonHeight);
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(m_time) * 2.4f);
    list->AddRectFilled(ImVec2(min.x - 4, min.y - 4), ImVec2(max.x + 4, max.y + 4),
                        ui::withAlpha(palette::kGold, 0.10f + 0.12f * pulse), buttonHeight * 0.5f + 4);
    // A rounded gradient: the pill's own vertices, shaded top to bottom.
    const int firstVertex = list->VtxBuffer.Size;
    list->AddRectFilled(min, max, IM_COL32_WHITE, buttonHeight * 0.5f);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(list, firstVertex, list->VtxBuffer.Size, min, ImVec2(min.x, max.y),
                                                  IM_COL32(255, 222, 120, 255), IM_COL32(222, 168, 46, 255));
    list->AddRect(min, max, hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 245, 210, 200),
                  buttonHeight * 0.5f, 2.0f);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
    const ImVec2 label = ImGui::CalcTextSize("Play");
    const float iconSize = em(1.3f);
    const float total = iconSize + em(0.5f) + label.x;
    const float x0 = min.x + (buttonWidth - total) * 0.5f;
    ui::drawIcon(list, Icon::Play, ImVec2(x0 + iconSize * 0.5f, min.y + buttonHeight * 0.5f), iconSize,
                 IM_COL32(40, 24, 0, 255));
    list->AddText(ImVec2(x0 + iconSize + em(0.5f), min.y + (buttonHeight - label.y) * 0.5f), IM_COL32(40, 24, 0, 255),
                  "Play");
    ImGui::PopFont();
    if (pressed)
        launchGame();
    if (!m_launchError.empty()) {
        ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
        wrapped(palette::kBad, m_launchError);
    }

    ImGui::Dummy(ImVec2(0.0f, em(1.6f)));
    const float row = em(31.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - row) * 0.5f));
    if (ui::iconButton("Game files", Icon::Folder) && m_live) {
        const std::string url = "file://" + pathUtf8(extractedDisc(utf8Path(m_config.workspace)));
        SDL_OpenURL(url.c_str());
    }
    ImGui::SameLine(0.0f, em(0.6f));
    if (ui::iconButton("Options", Icon::Interface))
        goTo(Page::Options);
    ImGui::SameLine(0.0f, em(0.6f));
    if (ui::iconButton("Rebuild", Icon::Gear))
        m_page = Page::Build;
    ImGui::Dummy(ImVec2(0.0f, em(1.6f)));
    centered("In the game, F1 opens the menu: display, sound, controls and more.", palette::kTextMuted, 1.0f);
}

} // namespace dq8::launcher
