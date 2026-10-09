# dq8-launcher -- takes the player's disc image to a playable game: checks the
# disc and the tools, builds everything, starts the game. Needs no game files,
# no translated code and no renderer, so it builds in seconds on its own:
#
#   cmake -S tools/launcher -B build/launcher -G Ninja
#   cmake --build build/launcher
#   ./build/launcher/dq8-launcher

cmake_minimum_required(VERSION 3.24)
project(dq8_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

get_filename_component(DQ8_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(DQ8_IMGUI_DIR "${DQ8_ROOT}/thirdparty/imgui" CACHE PATH "Dear ImGui's sources")
if(NOT EXISTS "${DQ8_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui not found under ${DQ8_IMGUI_DIR}. "
                        "Run: git submodule update --init thirdparty/imgui")
endif()

find_package(SDL3 3.2 CONFIG QUIET)
if(TARGET SDL3::SDL3)
    set(DQ8_LAUNCHER_SDL SDL3::SDL3)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DQ8_LAUNCHER_SDL3 REQUIRED IMPORTED_TARGET sdl3>=3.2)
    set(DQ8_LAUNCHER_SDL PkgConfig::DQ8_LAUNCHER_SDL3)
endif()

# The in-game menu's look, without the renderer it draws over in the game.
set(DQ8_UI_FONT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/ui_font_data.cpp")
add_custom_command(
    OUTPUT "${DQ8_UI_FONT_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${DQ8_IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf"
            "-DOUTPUT=${DQ8_UI_FONT_SOURCE}" -DSYMBOL=kUiFontData -P "${DQ8_ROOT}/cmake/Dq8EmbedBinary.cmake"
    DEPENDS "${DQ8_IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf" "${DQ8_ROOT}/cmake/Dq8EmbedBinary.cmake"
    COMMENT "Embedding the launcher font"
    VERBATIM)
add_library(dq8_launcher_ui STATIC
    "${DQ8_IMGUI_DIR}/imgui.cpp"
    "${DQ8_IMGUI_DIR}/imgui_draw.cpp"
    "${DQ8_IMGUI_DIR}/imgui_tables.cpp"
    "${DQ8_IMGUI_DIR}/imgui_widgets.cpp"
    "${DQ8_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${DQ8_IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp"
    "${DQ8_ROOT}/src/ui/ui_glyphs.cpp"
    "${DQ8_ROOT}/src/ui/ui_settings.cpp"
    "${DQ8_ROOT}/src/ui/ui_style.cpp"
    "${DQ8_ROOT}/src/ui/ui_widgets.cpp"
    "${DQ8_ROOT}/src/gfx/backends/sdlgpu/sdlgpu_input.cpp"
    "${DQ8_ROOT}/src/gfx/backends/sdlgpu/sdlgpu_window_icon.cpp"
    "${DQ8_UI_FONT_SOURCE}"
)
target_include_directories(dq8_launcher_ui PUBLIC "${DQ8_ROOT}/src")
target_include_directories(dq8_launcher_ui SYSTEM PUBLIC "${DQ8_IMGUI_DIR}" "${DQ8_IMGUI_DIR}/backends")
target_compile_definitions(dq8_launcher_ui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(dq8_launcher_ui PUBLIC ${DQ8_LAUNCHER_SDL})
include("${DQ8_ROOT}/cmake/Dq8Icon.cmake")
dq8_embed_window_icon(dq8_launcher_ui)

# The parts with no window, which the tests exercise directly.
add_library(dq8_launcher_core STATIC
    launcher_iso.cpp
    launcher_json.cpp
    launcher_payload.cpp
    launcher_pipeline.cpp
    launcher_process.cpp
    launcher_sha256.cpp
    launcher_tools.cpp
)
target_include_directories(dq8_launcher_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(dq8_launcher_core PUBLIC ${DQ8_LAUNCHER_SDL})

include("${DQ8_ROOT}/cmake/Dq8Utf8CodePage.cmake")

add_executable(dq8-launcher WIN32 launcher_main.cpp launcher_app.cpp)
target_link_libraries(dq8-launcher PRIVATE dq8_launcher_core dq8_launcher_ui)
# It writes the game's settings, by the UTF-8 path SDL gives.
dq8_use_utf8_code_page(dq8-launcher)
if(APPLE)
    # A double-clickable app; it finds the source tree above itself.
    set_target_properties(dq8-launcher PROPERTIES
        MACOSX_BUNDLE ON
        OUTPUT_NAME "DQ8Recomp Launcher"
        MACOSX_BUNDLE_BUNDLE_NAME "DQ8Recomp Launcher"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.dq8recomp.launcher"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in")
endif()
# After MACOSX_BUNDLE: on macOS the icon goes in the bundle.
dq8_add_icon(dq8-launcher)

enable_testing()
add_executable(dq8-launcher-tests launcher_tests.cpp)
target_link_libraries(dq8-launcher-tests PRIVATE dq8_launcher_core dq8_launcher_ui)
dq8_use_utf8_code_page(dq8-launcher-tests)
add_test(NAME launcher COMMAND dq8-launcher-tests)
# Every page drawn once off screen; 77 (skipped) where there is no GPU.
add_test(NAME launcher-pages COMMAND dq8-launcher --smoke --repo "${DQ8_ROOT}")
set_tests_properties(launcher-pages PROPERTIES SKIP_RETURN_CODE 77)
if(NOT APPLE)
    set_tests_properties(launcher-pages PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=offscreen")
endif()
