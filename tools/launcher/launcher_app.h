// The launcher's window: from the player's disc image to a playable game in
// five pages, drawn in the in-game menu's style.
#pragma once

#include "launcher_pipeline.h"
#include "launcher_tools.h"
#include "ui/ui_settings.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace dq8::launcher {

enum class Page : uint8_t { Disc, Tools, Options, Build, Play, Count };

// What the launcher remembers between runs, beside the game's settings.ini.
struct LauncherConfig {
    std::string disc;
    std::string workspace;
    int jobs = 0; // 0 picks from the machine
};
std::filesystem::path configPath();
LauncherConfig loadConfig();
void saveConfig(const LauncherConfig &config);

// Compile jobs the machine can take: one a core, within memory.
int defaultJobs();

// Where a release keeps the game files by default: DQ8Recomp in the home folder.
std::string defaultGamesFolder();

// What the system's file pickers answered. They can answer on their own
// thread, even after the window has closed, so each holds a share of this
// rather than a pointer to the app.
struct DialogPicks {
    std::mutex mutex;
    std::optional<std::string> disc, workspace;
};

// Fixed states for the screenshots, drawn with no live work behind them.
enum class Preview : uint8_t {
    DiscEmpty, DiscReady, ToolsReady, ToolsMissing, Options, Building, BuildFailed, Play, Count
};
const char *previewName(Preview preview);

class LauncherApp {
public:
    // From a checkout, `repo` is it; from a release, `payload` carries the
    // source and tools, and the build runs in the game files' folder.
    // `overrides` replaces the remembered disc, folder and jobs where set;
    // without `persist`, nothing is written (screenshots of the live app).
    LauncherApp(std::filesystem::path repo, std::optional<Payload> payload, SDL_Window *window,
                const LauncherConfig &overrides = {}, bool persist = true);
    ~LauncherApp();

    void handleEvent(const SDL_Event &event);
    // Inside an ImGui frame.
    void draw();
    void showPreview(Preview preview);
    void setPage(Page page) { m_page = page; }
    bool checkingTools() const { return m_checkingTools.load(); }

private:
    void drawBackground();
    void drawSidebar(float width);
    void drawDiscPage();
    void drawToolsPage();
    void drawOptionsPage();
    void drawBuildPage();
    void drawLogTools();
    void drawPlayPage();
    void drawFooter();
    // What a bug report needs: the launcher, the machine, the steps and the
    // log's end, ready to paste into an issue.
    std::string bugReport() const;

    // Where the build runs and the game is started from.
    std::filesystem::path buildRepo() const;
    // Where the source is read before anything is built (hashes, the tree's parts).
    std::filesystem::path sourceRoot() const;
    // What a build must have been made from for Play to start it.
    BuiltFrom builtFrom() const;
    ChildEnvironment childEnvironment() const;

    void setDisc(const std::string &path);
    void checkToolsAsync();
    void runInstall(const InstallAction &action);
    void startBuild();
    void launchGame();
    void writeGameSettings();
    bool pageReachable(Page page) const;
    bool pageDone(Page page) const;
    void goTo(Page page);

    std::filesystem::path m_repo;
    std::optional<Payload> m_payload;
    SDL_Window *m_window = nullptr;
    LauncherConfig m_config;
    Page m_page = Page::Disc;
    bool m_live = true;
    bool m_persist = true;
    double m_time = 0.0;

    // Disc page.
    std::optional<DiscInfo> m_disc;
    bool m_dragging = false;

    // Tools page, filled by a worker.
    mutable std::mutex m_toolsMutex;
    std::optional<ToolReport> m_tools;
    std::atomic<bool> m_checkingTools{false};
    std::thread m_toolsThread;
    double m_copiedAt = -10.0;
    // "Install for me": the system's installer runs on a thread that can
    // outlive the window, so it shares this rather than the app. `stop` ends
    // the thread's watching before main quits SDL; the installer carries on.
    struct InstallState {
        std::mutex mutex;
        std::string line, error;
        std::atomic<bool> running{false}, finished{false}, openedInstaller{false}, stop{false};
    };
    std::shared_ptr<InstallState> m_install = std::make_shared<InstallState>();
    // Apple's installer works on its own once opened: the page checks again
    // every few seconds until the compiler appears.
    bool m_waitingForInstaller = false;
    double m_lastInstallCheck = 0.0;

    // Options page: the game's own settings, written before it starts.
    ui::Settings m_gameSettings;
    std::string m_gameSettingsPath;

    // Build page.
    Pipeline m_pipeline;
    bool m_showLog = false;
    // The page scrolls down to the log once it opens, as the steps fill it.
    bool m_revealLog = false;
    bool m_buildWasRunning = false;
    // The stopped build's log as one selectable text, rebuilt when the log
    // changes and not while the player is selecting in it.
    std::string m_logText;
    uint64_t m_logTextVersion = UINT64_MAX;
    bool m_logActive = false;
    double m_logCopiedAt = -10.0;
    std::array<StageState, kStageCount> m_previewStages{};
    std::string m_previewError;
    std::vector<std::string> m_previewLog;

    // Play page.
    std::string m_launchError;

    std::shared_ptr<DialogPicks> m_picks = std::make_shared<DialogPicks>();
    mutable bool m_built = false;
    mutable double m_builtCheckedAt = -1.0;
};

} // namespace dq8::launcher
