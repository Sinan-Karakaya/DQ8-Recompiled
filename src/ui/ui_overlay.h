// The in-game menu, drawn with Dear ImGui over the SDL GPU backend's window:
// a menu bar (F1, or Guide / Back+Start on a controller), the settings window,
// notifications and the frame-rate counter.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "ui/ui_glyphs.h"
#include "ui/ui_settings.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct ImGuiContext;

namespace dq8::ui {

// What the menu can ask of the game around it. A control whose function is
// empty is not shown.
struct HostServices {
    std::string version;
    std::function<void(float)> setVolume; // 0 to 1
    std::function<void(bool)> setPaused;
    // The game's speed against real time; 0 runs it as fast as it can go.
    std::function<void(double)> setSpeed;
    // Frames the game has finished drawing, for the counter; presents are
    // counted instead when it is empty.
    std::function<uint64_t()> completedRenders;
    // The guest's vsync clock, which DQ8_UI_SCRIPT is timed against, as
    // DQ8_PAD_SCRIPT is; presents are counted instead when it is empty.
    std::function<uint64_t()> guestFrame;
};

enum class SettingsPage : uint8_t { Display, Sound, Controls, Interface, Count };

class Overlay final : public gfx::SdlGpuOverlay {
public:
    Overlay(gfx::SdlGpuBackend &backend, Settings settings, std::string settingsPath, HostServices host);
    ~Overlay() override;
    Overlay(const Overlay &) = delete;
    Overlay &operator=(const Overlay &) = delete;

    // `format` is the texture format render() draws into.
    bool initialize(SDL_Window *window, SDL_GPUTextureFormat format, std::string &error);
    // Pushes every setting to the backend, the window and the host.
    void applySettings();

    bool handleEvent(const SDL_Event &event) override;
    void render(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width,
                uint32_t height) override;
    bool wantsFrames() override;
    bool capturesInput() const override;

    const Settings &settings() const { return m_settings; }
    void setMenuOpen(bool open);
    void openSettings(SettingsPage page);
    // Opens one of the menu bar's menus at the next frame, by its label.
    void showMenu(const char *label);
    void openAbout();
    void openShortcuts();
    // Picks the Controls page's controller or keyboard tab.
    void showBindings(bool keyboard);
    void notify(std::string text, Icon icon = Icon::Info, double seconds = 3.0);
    void setUserPaused(bool paused);
    void setFastForward(bool on);
    // Waits for the next key or controller input and binds it.
    void beginBindingCapture(gfx::PadInput input, bool keyboard, int slot);
    void takeScreenshot(bool withMenu);
    std::string screenshotFolder() const;

private:
    struct Toast {
        std::string text;
        Icon icon = Icon::Info;
        double startedAt = 0.0;
        double duration = 3.0;
    };
    struct Capture {
        bool active = false;
        gfx::PadInput input = gfx::PadInput::Count;
        bool keyboard = false;
        int slot = 0;
        double startedAt = 0.0;
    };
    struct ScriptStep {
        uint64_t frame = 0u;
        double seconds = -1.0; // wall time instead, for steps a pause would hold back
        std::string command;
        std::vector<std::string> arguments;
        bool done = false;
    };

    // DQ8_UI_SCRIPT (ui_script.cpp): menu actions on the guest frame clock,
    // for tests and screenshots of the menu over the running game.
    void loadScript();
    void runScript();
    void runStep(const ScriptStep &step, uint64_t frame);

    // Event side.
    bool hotkey(const SDL_KeyboardEvent &key);
    bool gamepadShortcut(const SDL_GamepadButtonEvent &button);
    bool captureEvent(const SDL_Event &event);
    bool backOut();
    void toggleMenu(bool fromController);

    // Frame side.
    void updateScale();
    void updateNavigation();
    void drawFrame();
    void drawMenuBar();
    void drawStatus();
    void drawSettingsWindow();
    void drawDisplayPage();
    void drawSoundPage();
    void drawControlsPage();
    void drawBindingTable(bool keyboard);
    void drawInterfacePage();
    void drawCapturePrompt();
    void drawAbout();
    void drawShortcuts();
    void drawStats();
    void pictureBounds(ImVec2 &min, ImVec2 &max) const;
    void drawFps();
    void drawSpeedBadge();
    void drawPauseBanner();
    void drawToasts();
    void updateCursor();
    void sampleFrameRate();
    void collectSavedScreenshots();

    // Settings side.
    void markDirty();
    void saveIfDirty(bool force);
    void applyVolume();
    void applyPause();
    void setFullscreen(bool fullscreen);
    void setPadConfig(const gfx::PadConfig &config);
    bool pausing() const;
    bool canPause() const { return static_cast<bool>(m_host.setPaused); }
    bool canChangeSpeed() const { return static_cast<bool>(m_host.setSpeed); }
    std::string speedLabel(float speed) const;
    SDL_GamepadType displayedControllerType() const;
    double now() const;

    gfx::SdlGpuBackend &m_backend;
    Settings m_settings;
    std::string m_settingsPath;
    HostServices m_host;
    SDL_Window *m_window = nullptr;
    ImGuiContext *m_context = nullptr;
    bool m_platformReady = false;
    bool m_rendererReady = false;
    float m_appliedScale = 0.0f;

    bool m_menuOpen = false;
    bool m_settingsOpen = false;
    bool m_focusSettings = false;
    SettingsPage m_page = SettingsPage::Display;
    bool m_aboutOpen = false;
    bool m_shortcutsOpen = false;
    bool m_statsOpen = false;
    std::string m_showMenu;
    bool m_windowFocused = true;
    bool m_userPaused = false;
    bool m_hostPaused = false;
    bool m_fastForward = false;
    bool m_gamepadNavHeld = false;
    bool m_keyboardBindings = false;
    int m_selectBindings = -1;
    gfx::PadInput m_hoveredInput = gfx::PadInput::Count;
    Capture m_capture;
    uint32_t m_pendingScale = 0u;
    double m_pendingScaleSince = 0.0;

    std::vector<Toast> m_toasts;
    std::vector<std::pair<SDL_JoystickID, std::string>> m_controllerNames;

    double m_rateStart = 0.0;
    uint64_t m_rateRenders = 0u;
    uint64_t m_presents = 0u;
    float m_fps = 0.0f;
    // Below the counter when they share a corner.
    float m_toastTop = 0.0f;
    float m_fpsLeftBottom = 0.0f;
    std::array<float, 64> m_fpsHistory{};
    size_t m_fpsHistoryNext = 0u;

    gfx::SdlGpuStats m_stats{};
    gfx::SdlGpuStats m_statsPrevious{};
    double m_statsAt = -1.0;
    double m_statsSpan = 0.0;
    double m_lastMouseMotion = 0.0;
    int m_uiScaleEdit = 0;

    bool m_dirty = false;
    double m_dirtyAt = 0.0;

    bool m_closePopups = false;
    std::vector<ScriptStep> m_script;
    double m_scriptStart = 0.0;

    struct Saver {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex m_savedMutex;
    std::vector<std::pair<bool, std::string>> m_saved;
    std::vector<Saver> m_savers;
};

} // namespace dq8::ui
