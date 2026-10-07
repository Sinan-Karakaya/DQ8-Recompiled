// Everything the in-game menu lets the player change, and its settings file.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "gfx/backends/sdlgpu/sdlgpu_window.h"

#include <cstdint>
#include <string>

namespace dq8::ui {

enum class Corner : uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

struct Settings {
    // Window
    bool fullscreen = false;
    int windowWidth = 0; // points; 0 picks a size from the display
    int windowHeight = 0;

    // Picture
    gfx::SdlGpuDisplayOptions display{};
    uint32_t resolutionScale = 1u;

    // Sound
    int volume = 100; // percent
    bool muted = false;
    bool muteInBackground = false;

    // Controls
    gfx::PadConfig pad = gfx::PadConfig::defaults();
    bool backStartOpensMenu = true;

    // Interface
    int uiScale = 100; // percent, on top of the display's own scale
    bool showFps = false;
    Corner fpsCorner = Corner::TopRight;
    bool notifications = true;
    bool pauseInMenu = true;
    bool pauseInBackground = false;
    bool hideIdleCursor = true;

    // Game
    float fastForwardSpeed = 2.0f; // 0 runs unpaced
};

std::string serializeSettings(const Settings &settings);
// Unknown or malformed entries keep their current value in `settings`.
void parseSettings(const std::string &text, Settings &settings);

// DQ8_SETTINGS_FILE, or settings.ini in SDL's per-user preferences folder.
std::string defaultSettingsPath();
// A missing file leaves the defaults; returns false only when it cannot be read.
bool loadSettings(const std::string &path, Settings &settings);
bool saveSettings(const std::string &path, const Settings &settings);

} // namespace dq8::ui
