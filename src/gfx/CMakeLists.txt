# Shared by the standalone GS dump harness and the game, so paths are
# relative to this file rather than either caller.

get_filename_component(DQ8_GFX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(DQ8_GFX_PS2X_RUNTIME "${DQ8_GFX_ROOT}/thirdparty/PS2Recomp/ps2xRuntime")
include("${DQ8_GFX_ROOT}/cmake/Dq8HostSimd.cmake")

if(NOT EXISTS "${DQ8_GFX_PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp")
    message(FATAL_ERROR
        "ps2xRuntime GS sources not found under ${DQ8_GFX_PS2X_RUNTIME}. "
        "Run: git submodule update --init --recursive")
endif()

# ---------------------------------------------------------------------------
# dq8_gfx_gs -- backend-neutral GS support: local memory, swizzled pixel
# access, page tracking and the transfer engine. No graphics API here, so it
# can be unit-tested against GSCpuBackend with no device present.
# ---------------------------------------------------------------------------
set(DQ8_GFX_GS_SOURCES
    gs/gs_vram.cpp
    gs/gs_transfer.cpp
    gs/gs_state.cpp
)
# GsVram's pixel accessors are backed by ps2xRuntime's swizzle tables. In the
# standalone dump-harness build nothing else compiles them, so they come in
# here; in the main build ps2_runtime already provides them and compiling them
# again would duplicate every GSMem symbol at the final link.
if(NOT TARGET ps2_runtime)
    list(APPEND DQ8_GFX_GS_SOURCES
        "${DQ8_GFX_PS2X_RUNTIME}/src/lib/gs/ps2_gs_memory.cpp")
endif()

add_library(dq8_gfx_gs STATIC ${DQ8_GFX_GS_SOURCES})
target_link_libraries(dq8_gfx_gs PUBLIC dq8_host_simd)

target_include_directories(dq8_gfx_gs PUBLIC "${DQ8_GFX_ROOT}/src")
target_include_directories(dq8_gfx_gs SYSTEM PUBLIC
    "${DQ8_GFX_PS2X_RUNTIME}/include"
    "${DQ8_GFX_PS2X_RUNTIME}/include/runtime"
)
if(NOT TARGET ps2_runtime)
    set_source_files_properties(
        "${DQ8_GFX_PS2X_RUNTIME}/src/lib/gs/ps2_gs_memory.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# ---------------------------------------------------------------------------
# dq8_gfx_sdlgpu -- the SDL3 GPU backend. Optional: without SDL3 the rest of
# the graphics tree, including the dump harness and the software backend,
# still builds.
# ---------------------------------------------------------------------------
option(DQ8_GFX_ENABLE_SDLGPU "Build the SDL3 GPU hardware GS backend" ON)

set(DQ8_GFX_SDL_TARGET "")
if(DQ8_GFX_ENABLE_SDLGPU)
    find_package(SDL3 3.2 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        set(DQ8_GFX_SDL_TARGET SDL3::SDL3)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(DQ8_GFX_SDL3 QUIET IMPORTED_TARGET sdl3>=3.2)
            if(TARGET PkgConfig::DQ8_GFX_SDL3)
                set(DQ8_GFX_SDL_TARGET PkgConfig::DQ8_GFX_SDL3)
            endif()
        endif()
    endif()
endif()

if(DQ8_GFX_SDL_TARGET)
    include("${DQ8_GFX_ROOT}/cmake/Dq8Shaders.cmake")

    add_library(dq8_gfx_sdlgpu STATIC
        backends/sdlgpu/sdlgpu_backend.cpp
        backends/sdlgpu/sdlgpu_device.cpp
        backends/sdlgpu/sdlgpu_display.cpp
        backends/sdlgpu/sdlgpu_input.cpp
        backends/sdlgpu/sdlgpu_targets.cpp
        backends/sdlgpu/sdlgpu_textures.cpp
    )
    target_link_libraries(dq8_gfx_sdlgpu PUBLIC dq8_gfx_gs ${DQ8_GFX_SDL_TARGET})

    dq8_add_shader(dq8_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_draw.vert"
        STAGE vert SYMBOL kGsDrawVertSpirv)
    dq8_add_shader(dq8_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_draw.frag"
        STAGE frag SYMBOL kGsDrawFragSpirv)
    dq8_add_shader(dq8_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_reinterpret.vert"
        STAGE vert SYMBOL kGsReinterpretVertSpirv)
    dq8_add_shader(dq8_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_reinterpret.frag"
        STAGE frag SYMBOL kGsReinterpretFragSpirv)
    dq8_add_shader(dq8_gfx_sdlgpu MSL_DECORATION_BINDING
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_display.frag"
        STAGE frag SYMBOL kGsDisplayFragSpirv)
    dq8_add_shader(dq8_gfx_sdlgpu MSL_DECORATION_BINDING
        SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_index8.frag"
        STAGE frag SYMBOL kGsIndex8FragSpirv)
    if(APPLE)
        dq8_add_shader(dq8_gfx_sdlgpu FRAMEBUFFER_FETCH
            SOURCE "${CMAKE_CURRENT_LIST_DIR}/backends/sdlgpu/shaders/gs_draw.frag"
            STAGE frag SYMBOL kGsDrawFetchSpirv)
    endif()

    message(STATUS "dq8 gfx: SDL3 GPU backend enabled")
elseif(DQ8_GFX_ENABLE_SDLGPU)
    message(STATUS "dq8 gfx: SDL3 >= 3.2 not found -- hardware backend disabled")
endif()
