#include "gfx/backends/sdlgpu/sdlgpu_menu.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dq8::gfx {

namespace {

constexpr uint32_t kMaxScale = 8u;

const char *aspectName(DisplayAspect aspect) {
    switch (aspect) {
    case DisplayAspect::Auto: return "Auto (game's Screen Size)";
    case DisplayAspect::Standard4x3: return "4:3";
    case DisplayAspect::Wide16x9: return "16:9 (stretched)";
    case DisplayAspect::SquarePixels: return "Square pixels (8:7)";
    case DisplayAspect::Fill: return "Fill window";
    }
    return "?";
}

const char *filterName(OutputFilter filter) {
    switch (filter) {
    case OutputFilter::Sharp: return "Sharp bilinear";
    case OutputFilter::Bilinear: return "Bilinear";
    case OutputFilter::Nearest: return "Nearest (integer scale)";
    }
    return "?";
}

const char *aspectKey(DisplayAspect aspect) {
    switch (aspect) {
    case DisplayAspect::Auto: return "auto";
    case DisplayAspect::Standard4x3: return "4:3";
    case DisplayAspect::Wide16x9: return "16:9";
    case DisplayAspect::SquarePixels: return "square";
    case DisplayAspect::Fill: return "fill";
    }
    return "auto";
}

const char *filterKey(OutputFilter filter) {
    switch (filter) {
    case OutputFilter::Sharp: return "sharp";
    case OutputFilter::Bilinear: return "bilinear";
    case OutputFilter::Nearest: return "nearest";
    }
    return "sharp";
}

} // namespace

std::string displaySettingsPath() {
    // For tests and side-by-side setups that must not touch the real file.
    if (const char *override = std::getenv("DQ8_SETTINGS_FILE"); override && *override)
        return override;
    char *folder = SDL_GetPrefPath("DQ8Recomp", "DQ8Recomp");
    if (!folder)
        return "dq8recomp-settings.ini";
    std::string path = std::string(folder) + "settings.ini";
    SDL_free(folder);
    return path;
}

DisplaySettings loadDisplaySettings() {
    DisplaySettings settings{};
    std::ifstream file(displaySettingsPath());
    std::string line;
    while (std::getline(file, line)) {
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1u);
        if (key == "internal_scale") {
            settings.internalScale = std::clamp<uint32_t>(
                static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10)), 1u, kMaxScale);
        } else if (key == "aspect") {
            for (auto aspect : {DisplayAspect::Auto, DisplayAspect::Standard4x3, DisplayAspect::Wide16x9,
                                DisplayAspect::SquarePixels, DisplayAspect::Fill})
                if (value == aspectKey(aspect))
                    settings.aspect = aspect;
        } else if (key == "filter") {
            for (auto filter : {OutputFilter::Sharp, OutputFilter::Bilinear, OutputFilter::Nearest})
                if (value == filterKey(filter))
                    settings.filter = filter;
        } else if (key == "remove_line_blend") {
            settings.removeLineBlend = value == "1";
        } else if (key == "fullscreen") {
            settings.fullscreen = value == "1";
        } else if (key == "show_fps") {
            settings.showFps = value == "1";
        }
    }
    return settings;
}

bool saveDisplaySettings(const DisplaySettings &settings) {
    const std::string path = displaySettingsPath();
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "[menu] cannot write %s\n", path.c_str());
        return false;
    }
    file << "# DQ8Recomp display settings, written by the in-game menu (F1).\n"
         << "internal_scale=" << settings.internalScale << '\n'
         << "aspect=" << aspectKey(settings.aspect) << '\n'
         << "filter=" << filterKey(settings.filter) << '\n'
         << "remove_line_blend=" << (settings.removeLineBlend ? 1 : 0) << '\n'
         << "fullscreen=" << (settings.fullscreen ? 1 : 0) << '\n'
         << "show_fps=" << (settings.showFps ? 1 : 0) << '\n';
    return static_cast<bool>(file);
}

SdlGpuMenu::~SdlGpuMenu() { shutdown(); }

bool SdlGpuMenu::initialize(SDL_Window *window, SDL_GPUDevice *device, std::string &error) {
    if (m_initialized)
        return true;
    m_window = window;
    m_device = device;
    m_settings = loadDisplaySettings();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    // The menu's layout is fixed; nothing to remember between runs.
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;

    if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
        error = "ImGui_ImplSDL3_InitForSDLGPU failed";
        ImGui::DestroyContext();
        return false;
    }
    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&info)) {
        error = "ImGui_ImplSDLGPU3_Init failed";
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    m_initialized = true;
    if (m_settings.fullscreen)
        SDL_SetWindowFullscreen(m_window, true);
    std::fprintf(stderr, "[menu] F1 opens the settings menu; settings in %s\n",
                 displaySettingsPath().c_str());
    return true;
}

void SdlGpuMenu::shutdown() {
    if (!m_initialized)
        return;
    SDL_WaitForGPUIdle(m_device);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

void SdlGpuMenu::toggle() {
    m_open = !m_open;
    m_justOpened = m_open;
}

bool SdlGpuMenu::handleEvent(const SDL_Event &event) {
    if (!m_initialized)
        return false;
    ImGui_ImplSDL3_ProcessEvent(&event);

    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat)
            return isOpen();
        if (event.key.scancode == SDL_SCANCODE_F1) {
            toggle();
            return true;
        }
        if (event.key.scancode == SDL_SCANCODE_F2) {
            m_testOpen = !m_testOpen;
            m_testJustOpened = m_testOpen;
            return true;
        }
        if (event.key.scancode == SDL_SCANCODE_F11) {
            m_settings.fullscreen = !m_settings.fullscreen;
            commit();
            return true;
        }
        if (isOpen() && event.key.scancode == SDL_SCANCODE_ESCAPE) {
            m_open = false;
            m_testOpen = false;
            return true;
        }
        return isOpen();
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return isOpen();
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
            m_backHeld = true;
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE ||
            (m_backHeld && event.gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
            toggle();
            return true;
        }
        return isOpen();
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
            m_backHeld = false;
        return isOpen();
    default:
        return false;
    }
}

bool SdlGpuMenu::takeScaleRequest(uint32_t &scale) {
    if (!m_scaleRequested)
        return false;
    m_scaleRequested = false;
    scale = m_settings.internalScale;
    return true;
}

void SdlGpuMenu::commit() {
    SDL_SetWindowFullscreen(m_window, m_settings.fullscreen);
    saveDisplaySettings(m_settings);
}

void SdlGpuMenu::drawMenu(uint32_t activeScale) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    // Focused on opening, so a controller can drive it at once.
    if (m_justOpened)
        ImGui::SetNextWindowFocus();
    m_justOpened = false;
    if (!ImGui::Begin("DQ8Recomp settings", &m_open,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    bool changed = false;

    ImGui::SeparatorText("Graphics");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    char current[64];
    std::snprintf(current, sizeof(current), "%ux (%ux%u)", m_settings.internalScale,
                  512u * m_settings.internalScale, 448u * m_settings.internalScale);
    if (ImGui::BeginCombo("Internal resolution", current)) {
        for (uint32_t scale = 1u; scale <= kMaxScale; ++scale) {
            char label[64];
            std::snprintf(label, sizeof(label), "%ux (%ux%u)%s", scale, 512u * scale, 448u * scale,
                          scale == 1u ? ", original" : "");
            if (ImGui::Selectable(label, scale == m_settings.internalScale) &&
                scale != m_settings.internalScale) {
                m_settings.internalScale = scale;
                m_scaleRequested = true;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (activeScale != m_settings.internalScale)
        ImGui::TextDisabled("Applying...");

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo("Aspect ratio", aspectName(m_settings.aspect))) {
        for (auto aspect : {DisplayAspect::Auto, DisplayAspect::Standard4x3, DisplayAspect::Wide16x9,
                            DisplayAspect::SquarePixels, DisplayAspect::Fill}) {
            if (ImGui::Selectable(aspectName(aspect), aspect == m_settings.aspect)) {
                changed |= aspect != m_settings.aspect;
                m_settings.aspect = aspect;
            }
        }
        ImGui::EndCombo();
    }
    // Widescreen is the game's own: it widens the view and lays out its
    // menus for 16:9. This only says which way it is set.
    const int wide = gameWidescreen();
    ImGui::TextDisabled("Game Screen Size: %s", wide == 1 ? "Wide Screen 16:9" : wide == 0 ? "Normal 4:3" : "not set yet");
    ImGui::TextDisabled("Widescreen: set Screen Size in the game's own Settings menu.");

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo("Upscaling filter", filterName(m_settings.filter))) {
        for (auto filter : {OutputFilter::Sharp, OutputFilter::Bilinear, OutputFilter::Nearest}) {
            if (ImGui::Selectable(filterName(filter), filter == m_settings.filter)) {
                changed |= filter != m_settings.filter;
                m_settings.filter = filter;
            }
        }
        ImGui::EndCombo();
    }

    changed |= ImGui::Checkbox("Sharper picture (remove the PS2's line blending)",
                               &m_settings.removeLineBlend);

    ImGui::SeparatorText("Window");
    changed |= ImGui::Checkbox("Fullscreen (F11)", &m_settings.fullscreen);
    changed |= ImGui::Checkbox("Show frame rate", &m_settings.showFps);

    ImGui::Spacing();
    ImGui::TextDisabled("Settings are saved automatically.");
    if (ImGui::Button("Close (F1)"))
        m_open = false;
    ImGui::End();

    if (changed)
        commit();
}

namespace {

// Case-insensitive: does `text` contain `filter`?
bool matchesFilter(const std::string &text, const char *filter) {
    if (!filter[0])
        return true;
    const std::string needle = filter;
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != text.end();
}

} // namespace

void SdlGpuMenu::drawTestMenu() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.8f, viewport->Size.y * 0.85f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (m_testJustOpened)
        ImGui::SetNextWindowFocus();
    m_testJustOpened = false;
    if (!ImGui::Begin("Test menu: events, story points, places (F2)", &m_testOpen,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    const std::shared_ptr<const TestMenuData> data = m_test;
    if (!data) {
        ImGui::TextUnformatted("Reading the game's debug lists...");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("For testing: this changes the game's progress. Don't save over a real adventure log after using it.");
    ImGui::TextDisabled("Use it while walking around (not in a battle, a menu or a conversation).");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
    ImGui::InputTextWithHint("##filter", "Filter (name, number or map)", m_testFilter, sizeof(m_testFilter));

    // Each action closes the menu, so the game goes on to it at once.
    bool acted = false;
    if (ImGui::BeginTabBar("##test")) {
        if (ImGui::BeginTabItem("Events")) {
            ImGui::Checkbox("First put the story at the start of the event's chapter", &m_testStoryFirst);
            ImGui::TextDisabled("Entries marked [setup] put the game in the state the events after them expect.");
            if (ImGui::BeginTable("##events", 4,
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                                  ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Chapter / no.", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Map / entry", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < data->events.size() && !acted; ++i) {
                    const TestMenuData::Event &event = data->events[i];
                    if (!matchesFilter(event.name, m_testFilter) && !matchesFilter(event.number, m_testFilter) &&
                        !matchesFilter(event.map, m_testFilter))
                        continue;
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Play")) {
                        if (m_testStoryFirst && data->setStory)
                            for (const TestMenuData::StoryPoint &point : data->storyPoints)
                                if (point.chapter == event.chapter) {
                                    data->setStory(point.chapter, point.step);
                                    break;
                                }
                        if (data->warp)
                            data->warp(event.map, event.program);
                        acted = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%2d  %s", event.chapter, event.number.c_str());
                    ImGui::TableNextColumn();
                    if (event.setup)
                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "[setup] %s", event.name.c_str());
                    else
                        ImGui::TextUnformatted(event.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s %d", event.map.c_str(), event.program);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Story points")) {
            ImGui::TextDisabled("Sets the story's progress as the game does on reaching that point; it takes effect"
                                " on the next map you enter.");
            if (ImGui::BeginChild("##story", ImVec2(0.0f, 0.0f))) {
                for (size_t i = 0; i < data->storyPoints.size() && !acted; ++i) {
                    const TestMenuData::StoryPoint &point = data->storyPoints[i];
                    if (!matchesFilter(point.name, m_testFilter))
                        continue;
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("Set") && data->setStory) {
                        data->setStory(point.chapter, point.step);
                        acted = true;
                    }
                    ImGui::SameLine();
                    ImGui::Text("%2d-%-2d  %s", point.chapter, point.step, point.name.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Places")) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            ImGui::InputText("Map", m_testMap, sizeof(m_testMap));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
            ImGui::InputInt("Entry", &m_testProgram);
            ImGui::SameLine();
            const bool known = !data->hasMap || data->hasMap(m_testMap);
            ImGui::BeginDisabled(!known);
            if (ImGui::Button("Go") && data->warp) {
                data->warp(m_testMap, m_testProgram);
                acted = true;
            }
            ImGui::EndDisabled();
            if (!known)
                ImGui::TextDisabled("No map called \"%s\".", m_testMap);
            ImGui::TextDisabled("Entry 100 arrives the usual way; an entry the map's script lacks can hang the game.");
            if (ImGui::BeginChild("##places", ImVec2(0.0f, 0.0f))) {
                for (size_t i = 0; i < data->places.size() && !acted; ++i) {
                    const TestMenuData::Place &place = data->places[i];
                    if (!matchesFilter(place.name, m_testFilter) && !matchesFilter(place.map, m_testFilter))
                        continue;
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("Go") && data->warp) {
                        data->warp(place.map, 100);
                        acted = true;
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", place.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", place.map.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    if (acted)
        m_testOpen = false;
}

void SdlGpuMenu::drawFps(double rendersPerSecond) {
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    if (rendersPerSecond >= 0.0)
        ImGui::Text("%.1f FPS", rendersPerSecond);
    else
        ImGui::Text("-- FPS");
    ImGui::End();
}

void SdlGpuMenu::render(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target,
                        double rendersPerSecond, uint32_t activeScale) {
    if (!visible())
        return;

    // Size the UI to the window, so it stays readable fullscreen at 4K. In
    // logical units: ImGui applies the display's pixel density itself.
    int width = 0, height = 0;
    SDL_GetWindowSize(m_window, &width, &height);
    const float scale = std::clamp(std::round(height / 360.0f) / 2.0f, 1.0f, 4.0f);
    ImGuiStyle &style = ImGui::GetStyle();
    if (style.FontScaleMain != scale) {
        ImGuiStyle fresh;
        ImGui::StyleColorsDark(&fresh);
        fresh.WindowRounding = 6.0f;
        fresh.FrameRounding = 4.0f;
        fresh.ScaleAllSizes(scale);
        fresh.FontScaleMain = scale;
        style = fresh;
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (m_open)
        drawMenu(activeScale);
    if (m_testOpen)
        drawTestMenu();
    if (m_settings.showFps)
        drawFps(rendersPerSecond);
    ImGui::Render();

    ImDrawData *drawData = ImGui::GetDrawData();
    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f)
        return;
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commands);
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    info.load_op = SDL_GPU_LOADOP_LOAD;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &info, 1u, nullptr);
    if (!pass)
        return;
    ImGui_ImplSDLGPU3_RenderDrawData(drawData, commands, pass);
    SDL_EndGPURenderPass(pass);
}

} // namespace dq8::gfx
