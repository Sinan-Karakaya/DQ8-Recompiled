// dq8-ui-preview --out DIR [--width W --height H] [--hidpi] | --smoke
// Draws the in-game menu off screen over a painted backdrop and saves each
// view as a PNG; nothing of the game is in them, so they can go where game
// captures cannot. --smoke only checks that every view draws.
#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "ui/ui_overlay.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace dq8;

namespace {
constexpr uint32_t kGameWidth = 512u, kGameHeight = 448u;

float mixf(float a, float b, float t) { return a + (b - a) * t; }
float smooth(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A dusk landscape: sky, sun, mountains and two rows of hills.
std::vector<uint8_t> paintBackdrop() {
    std::vector<uint8_t> pixels(kGameWidth * kGameHeight * 4u);
    for (uint32_t y = 0; y < kGameHeight; ++y) {
        for (uint32_t x = 0; x < kGameWidth; ++x) {
            const float u = x / float(kGameWidth), v = y / float(kGameHeight);
            float r = mixf(0.10f, 0.98f, smooth(0.0f, 0.62f, v));
            float g = mixf(0.16f, 0.62f, smooth(0.0f, 0.62f, v));
            float b = mixf(0.42f, 0.42f, smooth(0.0f, 0.62f, v));
            const float sunDistance = std::hypot(u - 0.68f, (v - 0.47f) * 0.875f);
            const float glow = std::exp(-sunDistance * 9.0f);
            r += glow * 0.5f;
            g += glow * 0.35f;
            b += glow * 0.1f;
            if (sunDistance < 0.055f)
                r = 1.0f, g = 0.93f, b = 0.72f;
            const auto ridge = [&](float base, float a, float f1, float f2, float phase) {
                return base + a * (std::sin(u * f1 + phase) * 0.6f + std::sin(u * f2 + phase * 1.7f) * 0.4f);
            };
            const float mountains = ridge(0.5f, 0.06f, 9.0f, 23.0f, 1.3f);
            const float hills = ridge(0.66f, 0.035f, 5.0f, 13.0f, 0.4f);
            const float near = ridge(0.8f, 0.03f, 3.0f, 8.0f, 2.2f);
            if (v > mountains)
                r = 0.22f + glow * 0.2f, g = 0.24f + glow * 0.12f, b = 0.42f;
            if (v > hills) {
                const float shade = (v - hills) * 2.5f;
                r = 0.20f - shade * 0.1f, g = 0.42f - shade * 0.12f, b = 0.26f - shade * 0.08f;
            }
            if (v > near) {
                const float shade = (v - near) * 2.0f;
                r = 0.12f, g = 0.32f - shade * 0.15f, b = 0.17f;
                // A path winding toward the hills.
                const float path = 0.5f + 0.18f * std::sin(v * 14.0f) * (1.0f - v);
                if (std::fabs(u - path) < 0.02f + (v - near) * 0.25f)
                    r = 0.62f, g = 0.52f, b = 0.36f;
            }
            uint8_t *p = &pixels[(y * kGameWidth + x) * 4u];
            p[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            p[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            p[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            p[3] = 255u;
        }
    }
    return pixels;
}

SDL_GPUTexture *texture(SDL_GPUDevice *device, uint32_t width, uint32_t height, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = usage;
    return SDL_CreateGPUTexture(device, &info);
}

bool upload(SDL_GPUDevice *device, SDL_GPUTexture *target, const std::vector<uint8_t> &pixels) {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<uint32_t>(pixels.size());
    SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(device, &info);
    if (!buffer)
        return false;
    std::memcpy(SDL_MapGPUTransferBuffer(device, buffer, false), pixels.data(), pixels.size());
    SDL_UnmapGPUTransferBuffer(device, buffer);
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = buffer;
    SDL_GPUTextureRegion region{};
    region.texture = target;
    region.w = kGameWidth;
    region.h = kGameHeight;
    region.d = 1u;
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device, buffer);
    return submitted;
}

std::vector<uint8_t> download(SDL_GPUDevice *device, SDL_GPUTexture *source, uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4u);
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = static_cast<uint32_t>(pixels.size());
    SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(device, &info);
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureRegion region{};
    region.texture = source;
    region.w = width;
    region.h = height;
    region.d = 1u;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = buffer;
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    SDL_WaitForGPUFences(device, true, &fence, 1u);
    SDL_ReleaseGPUFence(device, fence);
    std::memcpy(pixels.data(), SDL_MapGPUTransferBuffer(device, buffer, false), pixels.size());
    SDL_UnmapGPUTransferBuffer(device, buffer);
    SDL_ReleaseGPUTransferBuffer(device, buffer);
    for (size_t alpha = 3u; alpha < pixels.size(); alpha += 4u)
        pixels[alpha] = 0xFFu;
    return pixels;
}

void pushKey(SDL_Window *window, SDL_Scancode scancode, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.scancode = scancode;
    event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    event.key.down = down;
    SDL_PushEvent(&event);
}
} // namespace

int main(int argc, char **argv) {
    std::string out;
    bool smoke = false;
    bool hidpi = false;
    int width = 1280, height = 960;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (arg == "--smoke")
            smoke = true;
        else if (arg == "--hidpi")
            hidpi = true;
        else if (arg == "--width" && i + 1 < argc)
            width = std::atoi(argv[++i]);
        else if (arg == "--height" && i + 1 < argc)
            height = std::atoi(argv[++i]);
    }
    if (out.empty() && !smoke) {
        std::fprintf(stderr, "usage: dq8-ui-preview --out DIR [--width W --height H] [--hidpi] | --smoke\n");
        return 2;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SKIP: SDL_Init: %s\n", SDL_GetError());
        return 77;
    }
    std::string error;
    auto backend = gfx::createSdlGpuBackend(error);
    if (!backend) {
        std::fprintf(stderr, "SKIP: %s\n", error.c_str());
        return 77;
    }
    SDL_GPUDevice *device = backend->gpuDevice();
    SDL_Window *window = SDL_CreateWindow("DQ8Recomp menu preview", width, height,
                                          SDL_WINDOW_HIDDEN | (hidpi ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0));
    if (!window) {
        std::fprintf(stderr, "SKIP: SDL_CreateWindow: %s\n", SDL_GetError());
        return 77;
    }
    int pixelWidth = width, pixelHeight = height;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
    backend->padInput().initialize(error);

    // A DualSense, so the controller pages show a real controller's marks.
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x054C;
    desc.product_id = 0x0CE6;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    desc.name = "DualSense Wireless Controller";
    const SDL_JoystickID virtualId = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick *joystick = virtualId ? SDL_OpenJoystick(virtualId) : nullptr;
    if (joystick) {
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    }

    const std::filesystem::path folder = out.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(out);
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    const std::string settingsPath = (folder / "preview-settings.ini").string();
    std::filesystem::remove(settingsPath, ignored);

    ui::Settings settings;
    settings.showFps = true;
    ui::HostServices host;
    host.version = "preview";
    host.setVolume = [](float) {};
    host.setPaused = [](bool) {};
    // A game holding its 30 frames a second, times the speed the menu sets;
    // unlimited is shown as 8x.
    auto speed = std::make_shared<double>(1.0);
    host.setSpeed = [speed](double value) { *speed = value > 0.0 ? value : 8.0; };
    host.completedRenders = [speed, last = SDL_GetTicksNS(), renders = 0.0]() mutable {
        const uint64_t ticks = SDL_GetTicksNS();
        renders += static_cast<double>(ticks - last) * 30.0 * *speed / static_cast<double>(SDL_NS_PER_SECOND);
        last = ticks;
        return static_cast<uint64_t>(renders);
    };
    ui::Overlay overlay(*backend, settings, settingsPath, host);
    if (!overlay.initialize(window, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 1;
    }
    overlay.applySettings();

    SDL_GPUTexture *backdrop = texture(device, kGameWidth, kGameHeight, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUTexture *target = texture(device, pixelWidth, pixelHeight,
                                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!backdrop || !target || !upload(device, backdrop, paintBackdrop())) {
        std::fprintf(stderr, "FAIL: preview textures: %s\n", SDL_GetError());
        return 1;
    }

    // One frame as the game's window would show it: the picture fitted per
    // the display options, then the menu over it.
    const auto frame = [&](bool keep) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (!overlay.handleEvent(event))
                backend->padInput().handleEvent(event);
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
        const gfx::SdlGpuRect rect = gfx::sdlGpuDisplayRect(overlay.settings().display, kGameWidth, kGameHeight,
                                                            pixelWidth, pixelHeight, 0);
        SDL_GPUBlitInfo blit{};
        blit.source.texture = backdrop;
        blit.source.w = kGameWidth;
        blit.source.h = kGameHeight;
        blit.destination.texture = target;
        blit.destination.x = rect.x;
        blit.destination.y = rect.y;
        blit.destination.w = rect.width;
        blit.destination.h = rect.height;
        blit.load_op = SDL_GPU_LOADOP_CLEAR;
        blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);
        overlay.render(commands, target, pixelWidth, pixelHeight);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        SDL_WaitForGPUFences(device, true, &fence, 1u);
        SDL_ReleaseGPUFence(device, fence);
        SDL_Delay(keep ? 0u : 16u);
    };
    const auto settle = [&](int frames) {
        for (int i = 0; i < frames; ++i)
            frame(false);
    };
    int failures = 0;
    const auto shoot = [&](const char *name) {
        frame(true);
        if (smoke)
            return;
        const std::vector<uint8_t> pixels = download(device, target, pixelWidth, pixelHeight);
        SDL_Surface *surface = SDL_CreateSurfaceFrom(pixelWidth, pixelHeight, SDL_PIXELFORMAT_RGBA32,
                                                     const_cast<uint8_t *>(pixels.data()), pixelWidth * 4);
        const std::string path = (folder / (std::string(name) + ui::kImageExtension)).string();
        if (!surface || !ui::saveImage(surface, path.c_str())) {
            std::fprintf(stderr, "FAIL: could not save %s: %s\n", path.c_str(), SDL_GetError());
            ++failures;
        } else {
            std::printf("wrote %s\n", path.c_str());
        }
        SDL_DestroySurface(surface);
    };
    const auto press = [&](SDL_GamepadButton button, bool down) {
        if (joystick)
            SDL_SetJoystickVirtualButton(joystick, button, down);
    };

    settle(40);
    // The menu bar, with the Settings menu dropped down and its second entry
    // under the keyboard cursor.
    overlay.showMenu("Settings");
    settle(20);
    pushKey(window, SDL_SCANCODE_DOWN, true);
    pushKey(window, SDL_SCANCODE_DOWN, false);
    settle(4);
    pushKey(window, SDL_SCANCODE_DOWN, true);
    pushKey(window, SDL_SCANCODE_DOWN, false);
    settle(30);
    shoot("menu-bar");

    overlay.setMenuOpen(false);
    overlay.showMenu("Game");
    settle(20);
    pushKey(window, SDL_SCANCODE_DOWN, true);
    pushKey(window, SDL_SCANCODE_DOWN, false);
    settle(4);
    pushKey(window, SDL_SCANCODE_DOWN, true);
    pushKey(window, SDL_SCANCODE_DOWN, false);
    settle(30);
    shoot("game-menu");

    overlay.setMenuOpen(false);
    overlay.openSettings(ui::SettingsPage::Display);
    settle(30);
    shoot("settings-display");

    // Something held, so the pad diagram lights up. Only inputs the menu does
    // not navigate with: L1/R1 turn its pages and the left stick moves in it.
    const auto axis = [&](SDL_GamepadAxis which, Sint16 value) {
        if (joystick)
            SDL_SetJoystickVirtualAxis(joystick, which, value);
    };
    press(SDL_GAMEPAD_BUTTON_START, true);
    press(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    axis(SDL_GAMEPAD_AXIS_RIGHTX, 24000);
    axis(SDL_GAMEPAD_AXIS_RIGHTY, -16000);
    overlay.openSettings(ui::SettingsPage::Controls);
    settle(30);
    shoot("settings-controls");
    press(SDL_GAMEPAD_BUTTON_START, false);
    press(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
    axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    axis(SDL_GAMEPAD_AXIS_RIGHTX, 0);
    axis(SDL_GAMEPAD_AXIS_RIGHTY, 0);

    overlay.beginBindingCapture(gfx::PadInput::Triangle, false, 0);
    settle(20);
    shoot("binding-capture");
    overlay.setMenuOpen(false);

    overlay.openSettings(ui::SettingsPage::Controls);
    overlay.showBindings(true);
    settle(20);
    shoot("settings-keyboard");

    overlay.openSettings(ui::SettingsPage::Sound);
    settle(20);
    shoot("settings-sound");
    overlay.openSettings(ui::SettingsPage::Interface);
    settle(20);
    shoot("settings-interface");

    overlay.setMenuOpen(false);
    overlay.openAbout();
    settle(20);
    shoot("about");
    overlay.setMenuOpen(false);
    overlay.openShortcuts();
    settle(20);
    shoot("shortcuts");

    // In play: no menu, the frame-rate counter, fast-forward and a notification.
    overlay.setMenuOpen(false);
    overlay.setFastForward(true);
    overlay.notify("Screenshot saved: DQ8_2026-10-06_18-42-07.png", ui::Icon::Camera);
    settle(40);
    shoot("in-game-hud");
    overlay.setFastForward(false);
    overlay.setUserPaused(true);
    settle(20);
    shoot("paused");
    overlay.setUserPaused(false);

    if (joystick)
        SDL_CloseJoystick(joystick);
    if (virtualId)
        SDL_DetachVirtualJoystick(virtualId);
    SDL_ReleaseGPUTexture(device, backdrop);
    SDL_ReleaseGPUTexture(device, target);
    std::puts(failures == 0 ? "PASS: menu preview drew every view" : "FAIL: some views were not saved");
    return failures == 0 ? 0 : 1;
}
