# dq8_ui -- the in-game menu, drawn with Dear ImGui over the SDL GPU backend's
# window. Built wherever that backend is: the game links it, and the dump
# harness builds it for its tests and the menu preview.

if(NOT TARGET dq8_gfx_sdlgpu)
    return()
endif()

get_filename_component(DQ8_UI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(DQ8_IMGUI_DIR "${DQ8_UI_ROOT}/thirdparty/imgui")
if(NOT EXISTS "${DQ8_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui not found under ${DQ8_IMGUI_DIR}. "
        "Run: git submodule update --init thirdparty/imgui")
endif()

# Imported targets are local to the directory that found them.
if(DQ8_GFX_SDL_TARGET STREQUAL "SDL3::SDL3" AND NOT TARGET SDL3::SDL3)
    find_package(SDL3 3.2 CONFIG REQUIRED)
elseif(DQ8_GFX_SDL_TARGET STREQUAL "PkgConfig::DQ8_GFX_SDL3" AND NOT TARGET PkgConfig::DQ8_GFX_SDL3)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DQ8_GFX_SDL3 REQUIRED IMPORTED_TARGET sdl3>=3.2)
endif()

add_library(dq8_imgui STATIC
    "${DQ8_IMGUI_DIR}/imgui.cpp"
    "${DQ8_IMGUI_DIR}/imgui_draw.cpp"
    "${DQ8_IMGUI_DIR}/imgui_tables.cpp"
    "${DQ8_IMGUI_DIR}/imgui_widgets.cpp"
    "${DQ8_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${DQ8_IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp"
)
target_include_directories(dq8_imgui SYSTEM PUBLIC "${DQ8_IMGUI_DIR}" "${DQ8_IMGUI_DIR}/backends")
target_compile_definitions(dq8_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(dq8_imgui PUBLIC ${DQ8_GFX_SDL_TARGET})

# Roboto Medium from Dear ImGui's own fonts (Apache 2.0), compiled in so the
# menu needs no files beside the executable.
set(DQ8_UI_FONT "${DQ8_IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf")
set(DQ8_UI_FONT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/ui_font_data.cpp")
add_custom_command(
    OUTPUT "${DQ8_UI_FONT_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${DQ8_UI_FONT}" "-DOUTPUT=${DQ8_UI_FONT_SOURCE}"
            -DSYMBOL=kUiFontData -P "${DQ8_UI_ROOT}/cmake/Dq8EmbedBinary.cmake"
    DEPENDS "${DQ8_UI_FONT}" "${DQ8_UI_ROOT}/cmake/Dq8EmbedBinary.cmake"
    COMMENT "Embedding the menu font"
    VERBATIM)

add_library(dq8_ui STATIC
    ui_glyphs.cpp
    ui_overlay.cpp
    ui_pages.cpp
    ui_script.cpp
    ui_settings.cpp
    ui_style.cpp
    ui_widgets.cpp
    "${DQ8_UI_FONT_SOURCE}"
)
target_include_directories(dq8_ui PUBLIC "${DQ8_UI_ROOT}/src")
target_link_libraries(dq8_ui PUBLIC dq8_gfx_sdlgpu dq8_imgui)

message(STATUS "dq8 ui: in-game menu enabled (Dear ImGui)")
