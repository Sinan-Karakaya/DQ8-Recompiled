# DQ8_LINK_GENERATED links the translated game corpus and PS2 runtime.

add_executable(dq8
    main.cpp
)

target_compile_definitions(dq8 PRIVATE DQ8_VERSION="${DQ8_VERSION}")

# Overlays require separately translated corpora, so linking is opt-in.
option(DQ8_LINK_OVERLAYS
    "Link the six translated MWo3 overlays; requires DQ8_LINK_GENERATED=ON"
    OFF)

if(DQ8_LINK_GENERATED)
    target_sources(dq8 PRIVATE vu_bounds.cpp)
    if(NOT TARGET ps2_runtime)
        message(FATAL_ERROR
            "DQ8_LINK_GENERATED=ON but the ps2_runtime target does not exist. "
            "Check that thirdparty/PS2Recomp/ps2xRuntime configured successfully "
            "and its platform dependencies are installed.")
    endif()

    file(GLOB DQ8_GENERATED_SOURCES CONFIGURE_DEPENDS
        "${DQ8_GENERATED_DIR}/*.cpp")

    list(LENGTH DQ8_GENERATED_SOURCES DQ8_GENERATED_SOURCE_COUNT)
    if(DQ8_GENERATED_SOURCE_COUNT EQUAL 0)
        message(FATAL_ERROR
            "DQ8_LINK_GENERATED=ON but no .cpp files were found under "
            "${DQ8_GENERATED_DIR}. Regenerate with: "
            "./build/ps2recomp-standalone/ps2xRecomp/ps2_recomp "
            "config/SLUS_212.07/dq8.toml")
    endif()
    message(STATUS
        "dq8: linking ${DQ8_GENERATED_SOURCE_COUNT} generated translation "
        "units from ${DQ8_GENERATED_DIR}")

    # Isolate corpus optimization flags and rebuilds from the launcher.
    add_library(dq8_generated STATIC ${DQ8_GENERATED_SOURCES})
    target_link_libraries(dq8_generated PRIVATE dq8_host_simd)

    target_include_directories(dq8_generated PRIVATE
        "${DQ8_GENERATED_DIR}"
        "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/include"
        "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/include/runtime"
        "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xIOP/include"
        "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/src/lib/Kernel"
    )

    separate_arguments(DQ8_GENERATED_OPTFLAGS_LIST UNIX_COMMAND "${DQ8_GENERATED_OPTFLAGS}")
    target_compile_options(dq8_generated PRIVATE ${DQ8_GENERATED_OPTFLAGS_LIST})
    # Every function includes the same runtime and large generated declarations.
    target_precompile_headers(dq8_generated PRIVATE
        <ps2_runtime_macros.h> <ps2_runtime.h>
        <ps2_recompiled_functions.h> <ps2_recompiled_stubs.h>
        <ps2_syscalls.h> <ps2_stubs.h>)

    # Prefer lld for the large generated corpus.
    find_program(DQ8_LLD_PROGRAM ld.lld)
    if(DQ8_LLD_PROGRAM AND NOT APPLE)
        message(STATUS "dq8: using lld (${DQ8_LLD_PROGRAM}) to link the generated corpus")
        target_link_options(dq8 PRIVATE -fuse-ld=lld)
    endif()

    target_link_libraries(dq8 PRIVATE dq8_generated ps2_runtime)
    target_compile_definitions(dq8 PRIVATE DQ8_LINK_GENERATED=1)

    # The hardware GS backend, when SDL3 was found. Selected at runtime with
    # --gs=sdlgpu; without it dq8 still runs on the software rasteriser.
    if(TARGET dq8_gfx_sdlgpu)
        target_link_libraries(dq8 PRIVATE dq8_gfx_sdlgpu)
        target_compile_definitions(dq8 PRIVATE DQ8_HAS_SDLGPU=1)
        message(STATUS "dq8: SDL GPU backend available (--gs=sdlgpu)")
    else()
        message(STATUS "dq8: SDL GPU backend not built; --gs=sdlgpu unavailable")
    endif()

    if(DQ8_LINK_OVERLAYS)
        add_subdirectory(overlay)
        target_link_libraries(dq8 PRIVATE dq8_overlay)
        # main.cpp guards its dq8::overlay::install(runtime) call on this.
        target_compile_definitions(dq8 PRIVATE DQ8_LINK_OVERLAYS=1)
    endif()
    # main.cpp includes ps2_recompiled_functions.h so it can name individual
    # recompiled functions -- --trace-printf redirects the stubbed debug-log
    # wrappers onto one of them.
    target_include_directories(dq8 PRIVATE "${DQ8_GENERATED_DIR}")
else()
    if(DQ8_LINK_OVERLAYS)
        message(FATAL_ERROR
            "DQ8_LINK_OVERLAYS=ON needs DQ8_LINK_GENERATED=ON: the overlay manager "
            "hooks a function (0x00100320) that lives in the main recompiled corpus.")
    endif()
    message(STATUS "dq8: DQ8_LINK_GENERATED=OFF -- building stub executable only")
endif()

if(APPLE AND DQ8_LINK_GENERATED)
    set(DQ8_MACOS_ELF "${CMAKE_SOURCE_DIR}/../Extracted_Usa/SLUS_212.07"
        CACHE FILEPATH "User-supplied ELF for the local macOS launcher")
    set(DQ8_MACOS_ISO "${CMAKE_SOURCE_DIR}/../DQ8_USA.iso"
        CACHE FILEPATH "User-supplied ISO for the local macOS launcher")
    set(DQ8_MACOS_BINARY "$<TARGET_FILE:dq8>")
    set(DQ8_MACOS_APP "${CMAKE_BINARY_DIR}/DQ8Recomp.app")
    configure_file("${CMAKE_SOURCE_DIR}/tools/macos/Info.plist"
        "${DQ8_MACOS_APP}/Contents/Info.plist" COPYONLY)
    configure_file("${CMAKE_SOURCE_DIR}/tools/macos/launch.sh.in"
        "${CMAKE_CURRENT_BINARY_DIR}/macos-launch.sh" @ONLY)
    file(GENERATE OUTPUT "${DQ8_MACOS_APP}/Contents/MacOS/DQ8Recomp"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/macos-launch.sh"
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
