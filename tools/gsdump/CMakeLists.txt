# dq8-gsreplay -- standalone GS-dump replay + image-diff harness.
# Builds the shared GS frontend and renderer without a game corpus or EE runtime.
#
#   cmake -S tools/gsdump -B build/gsdump -G Ninja
#   cmake --build build/gsdump
#   ./build/gsdump/dq8-gsreplay info <dump.gs.zst>

cmake_minimum_required(VERSION 3.24)
project(dq8_gsdump LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Locate the repository root whether this is configured standalone or as a
# subdirectory of the main project.
get_filename_component(DQ8_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PS2X_RUNTIME "${DQ8_ROOT}/thirdparty/PS2Recomp/ps2xRuntime")
if(NOT EXISTS "${PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp")
    message(FATAL_ERROR
        "ps2xRuntime GS sources not found under ${PS2X_RUNTIME}. "
        "Run: git submodule update --init --recursive")
endif()

# Everything below is optional. With none of it present the tool still builds
# and works: the PNG writer falls back to stored deflate blocks, and .gs.zst /
# .gs.xz dumps are streamed through the `zstd` / `xz` command-line tools.
find_package(ZLIB QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
    pkg_check_modules(LZMA IMPORTED_TARGET liblzma)
endif()

# The renderer itself: dq8_gfx_gs always, dq8_gfx_sdlgpu when SDL3 is present.
add_subdirectory("${DQ8_ROOT}/src/gfx" "${CMAKE_CURRENT_BINARY_DIR}/gfx")

add_executable(dq8-gsreplay
    main.cpp
    guest_stubs.cpp
    "${DQ8_ROOT}/src/gfx/gsdump/gs_dump_file.cpp"
    "${DQ8_ROOT}/src/gfx/gsdump/gs_frame_image.cpp"
    "${DQ8_ROOT}/src/gfx/gsdump/gs_replay.cpp"
    # ps2xRuntime, compiled directly: the point of the harness is that our
    # frontend work and upstream's are the *same* code path.
    "${PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp"
    "${PS2X_RUNTIME}/src/lib/gs/gs_cpu_backend.cpp"
    # ps2_gs_memory.cpp comes in via dq8_gfx_gs, which also compiles it.
)

target_include_directories(dq8-gsreplay PRIVATE
    "${DQ8_ROOT}/src/gfx/gsdump"
    "${PS2X_RUNTIME}/include"
    "${PS2X_RUNTIME}/include/runtime"
)
target_link_libraries(dq8-gsreplay PRIVATE dq8_gfx_gs)
if(TARGET dq8_gfx_sdlgpu)
    add_executable(dq8-gs-trace-replay backend_trace_replay.cpp guest_stubs.cpp
        "${PS2X_RUNTIME}/src/lib/gs/gs_threaded_backend.cpp"
        "${DQ8_ROOT}/src/gfx/gsdump/gs_frame_image.cpp"
        "${PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp"
        "${PS2X_RUNTIME}/src/lib/gs/gs_cpu_backend.cpp")
    target_link_libraries(dq8-gs-trace-replay PRIVATE dq8_gfx_sdlgpu)
    target_link_libraries(dq8-gsreplay PRIVATE dq8_gfx_sdlgpu)
    target_compile_definitions(dq8-gsreplay PRIVATE DQ8_GSDUMP_HAS_SDLGPU=1)
endif()

if(TARGET ZLIB::ZLIB)
    target_link_libraries(dq8-gsreplay PRIVATE ZLIB::ZLIB)
    target_compile_definitions(dq8-gsreplay PRIVATE DQ8_GSDUMP_HAVE_ZLIB=1)
else()
    message(STATUS "dq8-gsreplay: zlib headers not found -- PNGs use stored deflate")
endif()

if(TARGET PkgConfig::ZSTD)
    target_link_libraries(dq8-gsreplay PRIVATE PkgConfig::ZSTD)
    target_compile_definitions(dq8-gsreplay PRIVATE DQ8_GSDUMP_HAVE_ZSTD=1)
else()
    message(STATUS "dq8-gsreplay: libzstd headers not found -- .gs.zst dumps go through the `zstd` CLI")
endif()
if(TARGET PkgConfig::LZMA)
    target_link_libraries(dq8-gsreplay PRIVATE PkgConfig::LZMA)
    target_compile_definitions(dq8-gsreplay PRIVATE DQ8_GSDUMP_HAVE_LZMA=1)
else()
    message(STATUS "dq8-gsreplay: liblzma headers not found -- .gs.xz dumps go through the `xz` CLI")
endif()

# Keep the runtime's existing warning policy when compiling its sources here.
set_source_files_properties(
    "${PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp"
    "${PS2X_RUNTIME}/src/lib/gs/gs_cpu_backend.cpp"
    PROPERTIES COMPILE_OPTIONS "-w")

find_package(Threads REQUIRED)
target_link_libraries(dq8-gsreplay PRIVATE Threads::Threads)
if(TARGET dq8-gs-trace-replay)
    target_link_libraries(dq8-gs-trace-replay PRIVATE Threads::Threads)
endif()

enable_testing()

add_executable(dq8-gs-threaded-tests gs_threaded_tests.cpp
    guest_stubs.cpp
    "${PS2X_RUNTIME}/src/lib/gs/gs_threaded_backend.cpp"
    "${PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp"
    "${PS2X_RUNTIME}/src/lib/gs/gs_cpu_backend.cpp")
target_include_directories(dq8-gs-threaded-tests PRIVATE "${PS2X_RUNTIME}/include")
target_link_libraries(dq8-gs-threaded-tests PRIVATE Threads::Threads dq8_gfx_gs)
add_test(NAME gs-threaded COMMAND dq8-gs-threaded-tests)
set_tests_properties(gs-threaded PROPERTIES TIMEOUT 30)

# The GS transfer paths need no device, so they are tested on their own.
add_executable(dq8-gs-transfer-tests
    gs_transfer_tests.cpp
    "${PS2X_RUNTIME}/src/lib/gs/gs_cpu_backend.cpp"
)
target_include_directories(dq8-gs-transfer-tests SYSTEM PRIVATE
    "${PS2X_RUNTIME}/include"
    "${PS2X_RUNTIME}/include/runtime"
)
target_link_libraries(dq8-gs-transfer-tests PRIVATE dq8_gfx_gs Threads::Threads)
set_source_files_properties(
    "${PS2X_RUNTIME}/src/lib/gs/gs_cpu_backend.cpp"
    PROPERTIES COMPILE_OPTIONS "-w")
add_test(NAME gs-transfer COMMAND dq8-gs-transfer-tests)

if(TARGET dq8_gfx_sdlgpu)
    add_executable(dq8-sdlgpu-input-tests sdlgpu_input_tests.cpp)
    target_link_libraries(dq8-sdlgpu-input-tests PRIVATE dq8_gfx_sdlgpu)
    add_test(NAME sdlgpu-input COMMAND dq8-sdlgpu-input-tests)
    add_executable(dq8-sdlgpu-raster-tests
        sdlgpu_raster_tests.cpp
        guest_stubs.cpp
        "${PS2X_RUNTIME}/src/lib/gs/gs_cpu_backend.cpp"
        # The frontend defines the GIF packet timing counters the backend
        # reports; it is not otherwise used by these tests.
        "${PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp"
    )
    target_include_directories(dq8-sdlgpu-raster-tests SYSTEM PRIVATE
        "${PS2X_RUNTIME}/include"
        "${PS2X_RUNTIME}/include/runtime"
    )
    target_link_libraries(dq8-sdlgpu-raster-tests PRIVATE
        dq8_gfx_sdlgpu dq8_gfx_gs Threads::Threads)
    add_test(NAME sdlgpu-raster COMMAND dq8-sdlgpu-raster-tests)
    add_executable(dq8-movie-upload-tests movie_upload_tests.cpp)
    target_link_libraries(dq8-movie-upload-tests PRIVATE dq8_gfx_sdlgpu)
    add_test(NAME movie-upload COMMAND dq8-movie-upload-tests)
    # Headless: the backend needs no window, only a GPU device.
    set_tests_properties(sdlgpu-raster movie-upload PROPERTIES SKIP_RETURN_CODE 77)
    if(NOT APPLE)
        set_tests_properties(sdlgpu-raster movie-upload PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen")
    endif()
endif()

# Replay every dump under DQ8_GSDUMP_DIR and diff each against its reference.
# Skips cleanly when no dumps are present, so it is safe in CI.
set(DQ8_GSDUMP_DIR "${DQ8_ROOT}/../gsdumps" CACHE PATH
    "Directory of PCSX2 .gs dumps used by the gs-replay-regression target")
# Hardware against software on every dump, gated on PSNR.
add_custom_target(gs-backend-regression
    COMMAND ${CMAKE_COMMAND} -E env DQ8_GSREPLAY=$<TARGET_FILE:dq8-gsreplay>
            bash "${DQ8_ROOT}/tools/gsdump/run_backend_regression.sh"
            "${DQ8_GSDUMP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/backend-regression"
    DEPENDS dq8-gsreplay
    COMMENT "Comparing the SDL GPU backend against the software rasteriser")

add_custom_target(gs-replay-regression
    COMMAND ${CMAKE_COMMAND} -E env DQ8_GSREPLAY=$<TARGET_FILE:dq8-gsreplay>
            bash "${DQ8_ROOT}/tools/gsdump/run_regression.sh"
            "${DQ8_GSDUMP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/regression"
    DEPENDS dq8-gsreplay
    COMMENT "Replaying GS dumps and diffing against PCSX2 references")
