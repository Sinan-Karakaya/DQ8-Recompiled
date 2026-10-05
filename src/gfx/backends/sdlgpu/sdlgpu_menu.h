#pragma once

// In-game settings menu for the SDL GPU backend, drawn with Dear ImGui over
// the presented frame. F1 (or a controller's Guide button, or Back+Start)
// opens it; while it is open the game sees no input. F2 opens the test menu,
// which jumps to the game's events, story points and places.
//
// Everything here runs on the presenting thread: SDL events, ImGui frames and
// the command buffer the frame is shown with.

#include <SDL3/SDL.h>

#include "gfx/test_menu.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dq8::gfx {

// How the game picture is fitted into the window.
enum class DisplayAspect : uint8_t {
    Auto,         // follows the game's own Screen Size setting; the default
    Standard4x3,  // what a PS2 shows on a 4:3 TV
    Wide16x9,     // stretched to 16:9
    SquarePixels, // the GS frame 1:1, as earlier builds showed it (8:7)
    Fill,         // the whole window, whatever its shape
};

// How the picture is scaled to its size in the window.
enum class OutputFilter : uint8_t {
    Sharp,    // nearest to the next integer multiple, then bilinear: crisp, even pixels
    Bilinear, // smooth
    Nearest,  // integer multiples only, nearest; may leave borders
};

struct DisplaySettings {
    uint32_t internalScale = 1u; // 1..8, a multiple of the GS's own resolution
    DisplayAspect aspect = DisplayAspect::Auto;
    OutputFilter filter = OutputFilter::Sharp;
    // Show the frame without the PS2's one-line display blend (deflicker).
    bool removeLineBlend = true;
    bool fullscreen = false;
    bool showFps = false;
};

// settings.ini in SDL's per-user preferences folder, or DQ8_SETTINGS_FILE when
// set (tests use it to keep away from the real one). Unknown keys are ignored
// and missing ones keep their defaults, so the file survives version changes.
std::string displaySettingsPath();
DisplaySettings loadDisplaySettings();
bool saveDisplaySettings(const DisplaySettings &settings);

class SdlGpuMenu {
public:
    SdlGpuMenu() = default;
    ~SdlGpuMenu();
    SdlGpuMenu(const SdlGpuMenu &) = delete;
    SdlGpuMenu &operator=(const SdlGpuMenu &) = delete;

    bool initialize(SDL_Window *window, SDL_GPUDevice *device, std::string &error);
    void shutdown();

    // Feeds the event to the menu. Returns true when it belongs to the menu
    // and must not reach the game.
    bool handleEvent(const SDL_Event &event);
    bool isOpen() const { return m_open || m_testOpen; }
    // Whether anything is drawn this frame (a menu or the FPS counter).
    bool visible() const { return m_initialized && (isOpen() || m_settings.showFps); }

    // Records the menu into `commands`, over `target`, which already holds the
    // frame. `rendersPerSecond` is the game's own frame rate, for the counter.
    void render(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, double rendersPerSecond,
                uint32_t activeScale);

    // The game's own widescreen setting: 1 for 16:9, 0 for 4:3, -1 unknown.
    // DisplayAspect::Auto follows it.
    void setGameWidescreenQuery(std::function<int()> query) { m_gameWidescreen = std::move(query); }
    int gameWidescreen() const { return m_gameWidescreen ? m_gameWidescreen() : -1; }

    // What the test menu (F2) lists; until it is set, F2 says it is loading.
    void setTestMenu(std::shared_ptr<const TestMenuData> data) { m_test = std::move(data); }

    DisplaySettings &settings() { return m_settings; }
    const DisplaySettings &settings() const { return m_settings; }

    // A resolution change the menu asked for and the backend has not applied.
    bool takeScaleRequest(uint32_t &scale);

private:
    void toggle();
    void drawMenu(uint32_t activeScale);
    void drawFps(double rendersPerSecond);
    void drawTestMenu();
    void commit();

    SDL_Window *m_window = nullptr;
    SDL_GPUDevice *m_device = nullptr;
    DisplaySettings m_settings{};
    bool m_initialized = false;
    bool m_open = false;
    bool m_justOpened = false;
    bool m_scaleRequested = false;
    bool m_backHeld = false;
    std::string m_iniPath;
    std::function<int()> m_gameWidescreen;

    std::shared_ptr<const TestMenuData> m_test;
    bool m_testOpen = false;
    bool m_testJustOpened = false;
    bool m_testStoryFirst = true;
    char m_testFilter[64] = {};
    char m_testMap[16] = "m01";
    int m_testProgram = 100;
};

} // namespace dq8::gfx
