// dq8-launcher [--repo DIR] [--disc ISO] [--workspace DIR] [--jobs N]
//              [--build | --snapshot DIR | --preview DIR | --smoke]
//
// The DQ8Recomp launcher: a window that takes the player's disc image, checks
// the tools, builds the game and starts it. --build runs the same build with
// no window, printing progress. --snapshot draws the live pages into PNGs
// without saving anything; --preview draws every page with fixed states for
// the documentation, and --smoke draws them once.
#include "gfx/backends/sdlgpu/sdlgpu_window.h"
#include "launcher_app.h"
#include "launcher_iso.h"
#include "ui/ui_style.h"
#include "ui/ui_widgets.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace dq8;
using namespace dq8::launcher;

namespace {
constexpr int kWidth = 1180, kHeight = 780;

bool isRepo(const std::filesystem::path &dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "setup.py", ec) &&
           std::filesystem::is_regular_file(dir / "config" / "SLUS_212.07" / "hashes.json", ec);
}

// The source tree: given, or above the executable (also from inside a macOS
// app bundle), or above the working directory.
std::optional<std::filesystem::path> findRepo(const std::string &given) {
    if (!given.empty())
        return isRepo(utf8Path(given)) ? std::optional(utf8Path(given)) : std::nullopt;
    std::vector<std::filesystem::path> starts;
    if (const char *base = SDL_GetBasePath())
        starts.push_back(utf8Path(base));
    std::error_code ec;
    starts.push_back(std::filesystem::current_path(ec));
    for (std::filesystem::path dir : starts) {
        for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
            if (isRepo(dir))
                return std::filesystem::weakly_canonical(dir, ec);
            if (dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
    }
    return std::nullopt;
}

bool saveImage(SDL_Surface *surface, const std::string &path) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    return SDL_SavePNG(surface, path.c_str());
#else
    return SDL_SaveBMP(surface, path.c_str());
#endif
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

volatile std::sig_atomic_t g_interrupted = 0;
void onInterrupt(int) { g_interrupted = 1; }

// The window's build, without the window: progress goes to stdout. Ctrl+C or
// SIGTERM cancel it like the window's button, so ninja does not outlive it.
int headlessBuild(const std::optional<std::filesystem::path> &checkout, const std::optional<Payload> &payload,
                  const LauncherConfig &config) {
    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);
    if (config.disc.empty()) {
        std::fprintf(stderr, "--build needs --disc\n");
        return 2;
    }
    Pipeline pipeline;
    PipelineOptions options;
    options.workspace = utf8Path(!config.workspace.empty() ? config.workspace
                                 : payload                 ? defaultGamesFolder()
                                                           : pathUtf8(checkout->parent_path()));
    options.repo = payload ? unpackedSource(options.workspace) : *checkout;
    options.payload = payload;
    options.disc = utf8Path(config.disc);
    options.jobs = config.jobs > 0 ? config.jobs : defaultJobs();
    std::printf("[launcher] building into %s with %d jobs%s\n", pathUtf8(options.workspace).c_str(), options.jobs,
                payload ? (", from release " + payload->version).c_str() : "");
    pipeline.start(options, payload ? payloadEnvironment(*payload, options.repo)
                                    : ChildEnvironment{extraToolDirs(), {}});
    std::array<StageState::Status, kStageCount> shown{};
    Uint64 lastProgress = 0;
    do {
        SDL_Delay(500);
        if (g_interrupted) {
            g_interrupted = 0;
            std::printf("[launcher] stopping\n");
            pipeline.cancel();
        }
        const auto stages = pipeline.stages();
        for (size_t i = 0; i < kStageCount; ++i) {
            const StageState &stage = stages[i];
            const bool changed = stage.status != shown[i];
            const bool progress = stage.status == StageState::Status::Running && SDL_GetTicks() - lastProgress > 30000;
            if (!changed && !progress)
                continue;
            if (progress)
                lastProgress = SDL_GetTicks();
            shown[i] = stage.status;
            std::printf("[launcher] %s: %s", stageTitle(static_cast<Stage>(i)), statusName(stage.status));
            if (!stage.detail.empty())
                std::printf(", %s", stage.detail.c_str());
            if (stage.status == StageState::Status::Running && stage.progress >= 0.0)
                std::printf(", %.0f%%", stage.progress * 100.0);
            if (stage.remaining >= 0.0)
                std::printf(", about %.0f min left", stage.remaining / 60.0);
            if (stage.status == StageState::Status::Done)
                std::printf(" in %.0f s", stage.seconds);
            std::printf("\n");
            std::fflush(stdout);
        }
    } while (pipeline.running());
    if (!pipeline.succeeded()) {
        std::printf("[launcher] FAILED: %s\n", pipeline.error().c_str());
        for (const std::string &line : pipeline.log(40))
            std::printf("  %s\n", line.c_str());
        return 1;
    }
    std::printf("[launcher] ready: %s\n", pathUtf8(gamePath(options.repo)).c_str());
    return 0;
}

// Shown when the launcher is not inside a DQ8Recomp source tree.
void drawNoRepo() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::em(30.0f), 0.0f));
    ui::beginWindow("DQ8Recomp", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("The launcher has to sit in the DQ8Recomp folder, the one with setup.py and the "
                           "config folder, or be started with --repo and that folder's path.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, ui::em(0.6f)));
    if (ui::iconButton("Quit", ui::Icon::Close, true)) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
    ImGui::End();
}
} // namespace

int main(int argc, char **argv) {
    std::string repoArg, previewDir, snapshotDir;
    LauncherConfig overrides;
    bool smoke = false, build = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--repo" && i + 1 < argc)
            repoArg = argv[++i];
        else if (arg == "--disc" && i + 1 < argc)
            overrides.disc = argv[++i];
        else if (arg == "--workspace" && i + 1 < argc)
            overrides.workspace = argv[++i];
        else if (arg == "--jobs" && i + 1 < argc)
            overrides.jobs = std::atoi(argv[++i]);
        else if (arg == "--build")
            build = true;
        else if (arg == "--snapshot" && i + 1 < argc)
            snapshotDir = argv[++i];
        else if (arg == "--preview" && i + 1 < argc)
            previewDir = argv[++i];
        else if (arg == "--smoke")
            smoke = true;
        else {
            std::fprintf(stderr, "usage: dq8-launcher [--repo DIR] [--disc ISO] [--workspace DIR] [--jobs N]\n"
                                 "                    [--build | --snapshot DIR | --preview DIR | --smoke]\n");
            return arg == "--help" ? 0 : 2;
        }
    }
    const bool snapshot = !snapshotDir.empty();
    const bool previewing = smoke || !previewDir.empty() || snapshot;

    SDL_SetAppMetadata("DQ8Recomp Launcher", "1.0", "org.dq8recomp.launcher");
    // A release builds from its payload; --repo picks a checkout over it.
    const std::optional<Payload> payload = repoArg.empty() ? findPayload() : std::nullopt;
    if (build) {
        SDL_Init(0);
        const std::optional<std::filesystem::path> repo = payload ? std::nullopt : findRepo(repoArg);
        if (!repo && !payload) {
            std::fprintf(stderr, "not inside a DQ8Recomp source tree; pass --repo\n");
            return 2;
        }
        const int status = headlessBuild(repo, payload, overrides);
        SDL_Quit();
        return status;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return previewing ? 77 : 1;
    }
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | (previewing ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE);
    SDL_Window *window = SDL_CreateWindow("DQ8Recomp Launcher", kWidth, kHeight, flags);
    SDL_GPUDevice *device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_DXIL, false, nullptr);
    if (!window || !device || !SDL_ClaimWindowForGPUDevice(device, window)) {
        std::fprintf(stderr, "no window or GPU: %s\n", SDL_GetError());
        return previewing ? 77 : 1;
    }
    if (!previewing)
        SDL_SetWindowMinimumSize(window, 900, 640);
    gfx::setWindowIcon(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ui::loadFonts();
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init;
    init.Device = device;
    init.ColorTargetFormat = previewing ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
                                        : SDL_GetGPUSwapchainTextureFormat(device, window);
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);
    {
        // A scaled desktop shows as the display scale; Retina density is ImGui's own.
        const float density = SDL_GetWindowPixelDensity(window), scale = SDL_GetWindowDisplayScale(window);
        ui::applyStyle(density > 0.0f && scale > 0.0f ? std::max(1.0f, scale / density) : 1.0f);
    }

    std::optional<std::filesystem::path> repo = payload ? std::nullopt : findRepo(repoArg);
    if (!repo && !payload && previewing)
        repo = std::filesystem::current_path();
    std::optional<LauncherApp> app;
    // Only a player's window saves: the smoke test and the screenshots must
    // leave launcher.ini and the game's settings alone.
    if (repo || payload)
        app.emplace(repo.value_or(std::filesystem::path()), payload, window, overrides, !previewing);

    int pixelWidth = 0, pixelHeight = 0;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
    SDL_GPUTexture *offscreen = nullptr;
    if (previewing) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = static_cast<uint32_t>(pixelWidth);
        info.height = static_cast<uint32_t>(pixelHeight);
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        offscreen = SDL_CreateGPUTexture(device, &info);
    }

    // One frame into `target`, or into the window when it is null.
    const auto frame = [&](SDL_GPUTexture *target) {
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (app)
            app->draw();
        else
            drawNoRepo();
        ImGui::Render();
        ImDrawData *data = ImGui::GetDrawData();
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
        if (!commands)
            return;
        SDL_GPUTexture *swapchain = target;
        if (!swapchain && (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain, nullptr, nullptr) ||
                           !swapchain)) {
            SDL_SubmitGPUCommandBuffer(commands);
            return;
        }
        ImGui_ImplSDLGPU3_PrepareDrawData(data, commands);
        SDL_GPUColorTargetInfo color{};
        color.texture = swapchain;
        color.clear_color = SDL_FColor{0.03f, 0.04f, 0.12f, 1.0f};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &color, 1u, nullptr)) {
            ImGui_ImplSDLGPU3_RenderDrawData(data, commands, pass);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(commands);
    };

    int status = 0;
    const auto settle = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            SDL_Event event;
            while (SDL_PollEvent(&event))
                ImGui_ImplSDL3_ProcessEvent(&event);
            frame(offscreen);
        }
    };
    // The offscreen picture, saved under `dir`.
    const auto shoot = [&](const std::string &dir, const std::string &name) {
        if (dir.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(utf8Path(dir), ec);
        std::vector<uint8_t> pixels = download(device, offscreen, uint32_t(pixelWidth), uint32_t(pixelHeight));
        SDL_Surface *surface =
            SDL_CreateSurfaceFrom(pixelWidth, pixelHeight, SDL_PIXELFORMAT_RGBA32, pixels.data(), pixelWidth * 4);
#if SDL_VERSION_ATLEAST(3, 4, 0)
        const std::string path = pathUtf8(utf8Path(dir) / (name + ".png"));
#else
        const std::string path = pathUtf8(utf8Path(dir) / (name + ".bmp"));
#endif
        if (!surface || !saveImage(surface, path)) {
            std::fprintf(stderr, "FAIL: could not save %s: %s\n", path.c_str(), SDL_GetError());
            status = 1;
        } else {
            std::printf("wrote %s\n", path.c_str());
        }
        SDL_DestroySurface(surface);
    };
    if (snapshot && app) {
        // The live pages: the tool check finished, nothing written anywhere.
        // Offscreen frames have no vsync, so the wait is in time, not frames.
        const Uint64 started = SDL_GetTicks();
        while (app->checkingTools() && SDL_GetTicks() - started < 30000) {
            settle(1);
            SDL_Delay(20);
        }
        for (const auto &[page, name] : {std::pair{Page::Disc, "live-disc"}, std::pair{Page::Tools, "live-tools"},
                                         std::pair{Page::Options, "live-options"}}) {
            app->setPage(page);
            settle(12);
            shoot(snapshotDir, name);
        }
    } else if (previewing) {
        for (int index = 0; index < static_cast<int>(Preview::Count); ++index) {
            const auto preview = static_cast<Preview>(index);
            app->showPreview(preview);
            settle(12);
            shoot(previewDir, previewName(preview));
        }
        if (status == 0)
            std::printf("PASS: launcher preview drew every page\n");
    } else {
        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    running = false;
                if (app)
                    app->handleEvent(event);
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(50);
                continue;
            }
            frame(nullptr);
        }
    }

    app.reset();
    if (offscreen)
        SDL_ReleaseGPUTexture(device, offscreen);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
