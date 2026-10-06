// The in-game menu's pure parts: the settings file and the picture's fit.
#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "gfx/backends/sdlgpu/sdlgpu_window.h"
#include "ui/ui_settings.h"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace dq8;

namespace {
void require(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}

std::string rect(const gfx::SdlGpuRect &r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.width) + "x" +
           std::to_string(r.height);
}

void fits(const gfx::SdlGpuDisplayOptions &options, uint32_t w, uint32_t h, int wide,
          gfx::SdlGpuRect expected, const char *what) {
    const gfx::SdlGpuRect got = gfx::sdlGpuDisplayRect(options, 512u, 448u, w, h, wide);
    require(got == expected, std::string(what) + ": got " + rect(got) + ", expected " + rect(expected));
}

void settingsRoundTrip() {
    ui::Settings settings;
    settings.fullscreen = true;
    settings.windowWidth = 1600;
    settings.windowHeight = 1200;
    settings.display.aspect = gfx::SdlGpuAspect::Native;
    settings.display.filter = gfx::SdlGpuFilter::Nearest;
    settings.display.integerScale = true;
    settings.display.presentMode = SDL_GPU_PRESENTMODE_MAILBOX;
    settings.resolutionScale = 3u;
    settings.volume = 42;
    settings.muted = true;
    settings.muteInBackground = true;
    settings.pad.keyboardEnabled = false;
    settings.pad.controllers = gfx::ControllerSelection::One;
    settings.pad.controllerGuid = "030000004c050000e60c000000016800";
    settings.pad.controllerName = "DualSense Wireless Controller";
    settings.pad.stickDeadZone = 0.2f;
    settings.pad.triggerThreshold = 0.5f;
    settings.pad.invertCameraY = true;
    // Key names that look like file syntax must survive: ";" starts a
    // comment line and "=" separates keys from values.
    settings.pad.slots(gfx::PadInput::Cross, true)[0] = gfx::PadBinding::key(SDL_SCANCODE_SEMICOLON);
    settings.pad.slots(gfx::PadInput::Cross, true)[1] = gfx::PadBinding::key(SDL_SCANCODE_EQUALS);
    settings.pad.slots(gfx::PadInput::Square, true)[1] = {};
    settings.pad.slots(gfx::PadInput::Circle, false)[1] = gfx::PadBinding::axis(SDL_GAMEPAD_AXIS_RIGHTY, -1);
    settings.backStartOpensMenu = false;
    settings.uiScale = 150;
    settings.showFps = true;
    settings.fpsCorner = ui::Corner::BottomLeft;
    settings.notifications = false;
    settings.pauseInMenu = false;
    settings.pauseInBackground = true;
    settings.hideIdleCursor = false;
    settings.fastForwardSpeed = 0.0f;

    const std::string text = ui::serializeSettings(settings);
    ui::Settings parsed;
    ui::parseSettings(text, parsed);
    require(ui::serializeSettings(parsed) == text, "settings survive a round trip:\n" + text);
    require(parsed.pad.slots(gfx::PadInput::Cross, true)[0] == gfx::PadBinding::key(SDL_SCANCODE_SEMICOLON),
            "a ';' key binding is not taken for a comment");
    require(parsed.pad.slots(gfx::PadInput::Cross, true)[1] == gfx::PadBinding::key(SDL_SCANCODE_EQUALS),
            "an '=' key binding keeps its value");
    require(!parsed.pad.slots(gfx::PadInput::Square, true)[1].bound(), "a cleared slot stays cleared");
}

void malformedSettings() {
    ui::Settings settings;
    ui::parseSettings("[display]\nscale = 99\naspect = sideways\n[sound]\nvolume = loud\n"
                      "[keyboard]\ncross.3 = key:A\nnothing.1 = key:A\n[mystery]\nkey = value\n",
                      settings);
    require(settings.resolutionScale == 8u, "an out-of-range scale is clamped");
    require(settings.display.aspect == gfx::SdlGpuAspect::Auto, "an unknown aspect keeps the default");
    require(settings.volume == 100, "a non-numeric volume keeps the default");
    require(settings.pad.keyboard == gfx::PadConfig::defaults().keyboard, "bad binding keys change nothing");
}

void defaultBindingsRoundTrip() {
    const gfx::PadConfig defaults = gfx::PadConfig::defaults();
    for (const bool keyboard : {false, true})
        for (size_t input = 0; input < gfx::kPadInputCount; ++input)
            for (const auto &binding : defaults.slots(static_cast<gfx::PadInput>(input), keyboard)) {
                const std::string text = gfx::padBindingToString(binding);
                require(gfx::padBindingFromString(text) == binding, "binding '" + text + "' round-trips");
            }
}

void displayFits() {
    gfx::SdlGpuDisplayOptions options;
    fits(options, 1280u, 960u, 0, {0u, 0u, 1280u, 960u}, "4:3 fills a 4:3 window");
    fits(options, 1920u, 1080u, -1, {240u, 0u, 1440u, 1080u}, "4:3 pillarboxed in 16:9");
    fits(options, 1920u, 1080u, 1, {0u, 0u, 1920u, 1080u}, "auto follows the game to 16:9");
    options.aspect = gfx::SdlGpuAspect::Wide;
    fits(options, 1280u, 960u, 0, {0u, 120u, 1280u, 720u}, "16:9 letterboxed in 4:3");
    options.aspect = gfx::SdlGpuAspect::Native;
    fits(options, 1280u, 960u, 0, {91u, 0u, 1097u, 960u}, "square pixels keep 8:7");
    options.integerScale = true;
    fits(options, 1280u, 960u, 0, {128u, 32u, 1024u, 896u}, "integer native is exact on both axes");
    options.aspect = gfx::SdlGpuAspect::Standard;
    fits(options, 1280u, 1024u, 0, {42u, 64u, 1195u, 896u}, "integer 4:3 snaps the height");
    fits(options, 400u, 300u, 0, {0u, 0u, 400u, 300u}, "integer scaling gives way below 1x");
    options.aspect = gfx::SdlGpuAspect::Stretch;
    fits(options, 1000u, 300u, 0, {0u, 0u, 1000u, 300u}, "stretch fills any window");
}
} // namespace

int main() try {
    settingsRoundTrip();
    malformedSettings();
    defaultBindingsRoundTrip();
    displayFits();
    std::puts("PASS: menu settings file, bindings and picture fit");
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
