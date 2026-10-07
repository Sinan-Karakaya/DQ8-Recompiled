// The launcher's window: from the player's disc image to a playable game in
// five pages, drawn in the in-game menu's style.
#pragma once

#include "launcher_pipeline.h"
#include "launcher_tools.h"
#include "ui/ui_settings.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
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

// Fixed states for the screenshots, drawn with no live work behind them.
enum class Preview : uint8_t {
    DiscEmpty, DiscReady, ToolsReady, ToolsMissing, Options, Building, BuildFailed, Play, Count
};
const char *previewName(Preview preview);

class LauncherApp {
public:
    // `overrides` replaces the remembered disc, folder and jobs where set;
    // without `persist`, nothing is written (screenshots of the live app).
    LauncherApp(std::filesystem::path repo, SDL_Window *window, const LauncherConfig &overrides = {},
                bool persist = true);
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
    void drawPlayPage();
    void drawFooter();

    void setDisc(const std::string &path);
    void checkToolsAsync();
    void startBuild();
    void launchGame();
    void writeGameSettings();
    bool pageReachable(Page page) const;
    bool pageDone(Page page) const;
    void goTo(Page page);

    std::filesystem::path m_repo;
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

    // Options page: the game's own settings, written before it starts.
    ui::Settings m_gameSettings;
    std::string m_gameSettingsPath;

    // Build page.
    Pipeline m_pipeline;
    bool m_showLog = false;
    std::array<StageState, kStageCount> m_previewStages{};
    std::string m_previewError;
    std::vector<std::string> m_previewLog;

    // Play page.
    std::string m_launchError;

    // Folder pickers answer on their own thread.
    std::mutex m_dialogMutex;
    std::optional<std::string> m_pickedDisc, m_pickedWorkspace;
};

} // namespace dq8::launcher
