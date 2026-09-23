# DQ8Recomp — native PC port of Dragon Quest 8 (PS2) via static recompilation.
# Licensed under GPL-3.0 (see LICENSE). No game code or assets are built into
# this tree; recompiled output is generated locally by setup.py into
# build/generated/ (gitignored) and linked against the runtime here.

cmake_minimum_required(VERSION 3.24)

# Respect explicit compiler choices; otherwise prefer clang for generated code.
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(DQ8_DEFAULT_CLANG clang)
    if(DQ8_DEFAULT_CLANG)
        set(CMAKE_C_COMPILER "${DQ8_DEFAULT_CLANG}" CACHE FILEPATH "C compiler")
    endif()
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(DQ8_DEFAULT_CLANGXX clang++)
    if(DQ8_DEFAULT_CLANGXX)
        set(CMAKE_CXX_COMPILER "${DQ8_DEFAULT_CLANGXX}" CACHE FILEPATH "C++ compiler")
    endif()
endif()

# Regeneration rewrites the corpus; ccache reuses unchanged function bodies.
find_program(DQ8_CCACHE ccache)
if(DQ8_CCACHE)
    message(STATUS "DQ8Recomp: using ccache at ${DQ8_CCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER "${DQ8_CCACHE}" CACHE STRING "C compiler launcher")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${DQ8_CCACHE}" CACHE STRING "C++ compiler launcher")
endif()

# Version: `git describe` when available, otherwise a dev placeholder.
set(DQ8_VERSION "0.0.0-dev")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                describe --tags --always --dirty
        OUTPUT_VARIABLE DQ8_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE DQ8_GIT_DESCRIBE_RESULT)
    if(DQ8_GIT_DESCRIBE_RESULT EQUAL 0 AND NOT DQ8_GIT_DESCRIBE STREQUAL "")
        set(DQ8_VERSION "${DQ8_GIT_DESCRIBE}")
    endif()
endif()

project(DQ8Recomp
    VERSION 0.0.0
    DESCRIPTION "Dragon Quest 8 static recompilation"
    LANGUAGES C CXX)

# Apple's ar does not accept Ninja's @response files for the 12k-file corpus.
if(APPLE AND DQ8_LINK_GENERATED)
    find_program(DQ8_LLVM_AR NAMES llvm-ar
        HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
    set(CMAKE_AR "${DQ8_LLVM_AR}")
endif()

message(STATUS "DQ8Recomp version: ${DQ8_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(DQ8_BUILD_PS2RECOMP
    "Build PS2Recomp tools/runtime; configure may fetch dependencies."
    OFF)

option(DQ8_LINK_GENERATED
    "Link the locally generated game corpus; enables DQ8_BUILD_PS2RECOMP."
    OFF)

set(DQ8_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/generated/SLUS_212.07"
    CACHE PATH "Directory of recompiled .cpp files produced by ps2_recomp.")

set(DQ8_GENERATED_SIMD_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    set(DQ8_GENERATED_SIMD_FLAGS "-msse4.1")
endif()
# Generated memory accesses alias across widths and guest arithmetic wraps.
# Its x86 intrinsics also require SSE4.1, independently of runtime target flags.
set(DQ8_GENERATED_OPTFLAGS "-O2 -g0 ${DQ8_GENERATED_SIMD_FLAGS} -fno-strict-aliasing -fwrapv" CACHE STRING
    "Space-separated corpus flags; retain aliasing, wraparound, and required SIMD flags.")

if(DQ8_LINK_GENERATED AND NOT DQ8_BUILD_PS2RECOMP)
    message(STATUS "DQ8_LINK_GENERATED=ON: forcing DQ8_BUILD_PS2RECOMP=ON")
    set(DQ8_BUILD_PS2RECOMP ON CACHE BOOL
        "Build the PS2Recomp submodule (forced on by DQ8_LINK_GENERATED)." FORCE)
endif()

# SIMDe supplies the generated x86 intrinsic operations on ARM64 hosts.
add_library(dq8_simde INTERFACE)
include(cmake/Dq8HostSimd.cmake)
target_include_directories(dq8_simde SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/simde")

if(DQ8_BUILD_PS2RECOMP)
    message(STATUS
        "DQ8_BUILD_PS2RECOMP=ON: see https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Building for host dependencies.")
    # The game does not need the standalone studio GUI or upstream test runner.
    set(PS2X_BUILD_STUDIO OFF CACHE BOOL "DQ8Recomp: studio GUI not required" FORCE)
    set(PS2X_BUILD_TEST OFF CACHE BOOL "DQ8Recomp: upstream tests not required" FORCE)
    if(DQ8_LINK_GENERATED)
        # setup.py runs the recompiler separately before this runtime build.
        set(PS2X_BUILD_RECOMP OFF CACHE BOOL
            "DQ8Recomp: use the standalone ps2_recomp binary" FORCE)
        set(PS2X_BUILD_ANALYZER OFF CACHE BOOL
            "DQ8Recomp: use the standalone ps2_analyzer binary" FORCE)
        set(PS2X_BUILD_RUNTIME ON CACHE BOOL
            "DQ8Recomp: need the ps2_runtime target to link dq8" FORCE)
        # The game decrypts MVI streams; FFmpeg decodes them. Without these
        # optional libraries the runtime reports undecodable movies and skips them.
        find_package(PkgConfig QUIET)
        set(DQ8_HAVE_FFMPEG OFF)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(DQ8_FFMPEG_PROBE QUIET
                libavcodec libavformat libavutil libswresample libswscale)
            if(DQ8_FFMPEG_PROBE_FOUND)
                set(DQ8_HAVE_FFMPEG ON)
            endif()
        endif()
        if(DQ8_HAVE_FFMPEG)
            message(STATUS "DQ8Recomp: FFmpeg found (libavcodec ${DQ8_FFMPEG_PROBE_libavcodec_VERSION}); MPEG video decode enabled")
        else()
            message(STATUS "DQ8Recomp: FFmpeg not found; movies will be skipped. "
                           "Install ffmpeg-devel (or ffmpeg-free-devel) to play them.")
        endif()
        set(PS2X_ENABLE_FFMPEG ${DQ8_HAVE_FFMPEG} CACHE BOOL
            "DQ8Recomp: FFmpeg-backed MPEG decode, enabled when the libs are present" FORCE)
        set(PS2X_ENABLE_DEBUG_UI OFF CACHE BOOL
            "DQ8Recomp: skip imgui/rlImGui, not needed to link ps2_runtime" FORCE)
    endif()
    add_subdirectory(thirdparty/PS2Recomp)

    if(DQ8_LINK_GENERATED AND TARGET ps2_runtime)
        # Keep --trace-printf available even when the compiler omits _DEBUG.
        target_compile_definitions(ps2_runtime PRIVATE RUNTIME_DECI2CALL)
    endif()
endif()

add_subdirectory(src)
add_subdirectory(tools/runtime EXCLUDE_FROM_ALL)
