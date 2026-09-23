# Link separately translated overlays through the runtime dispatch manager.

# Keep overlay tables outside the main corpus glob: their generated
# register_functions.cpp files define the same global symbols as the main ELF.
set(DQ8_OVERLAY_GENERATED_DIR
    "${CMAKE_SOURCE_DIR}/build/generated/SLUS_212.07-overlays"
    CACHE PATH
    "Parent directory of per-overlay ps2_recomp outputs, separate from DQ8_GENERATED_DIR")

# Loader-table order (see kOverlays in overlay_manager.cpp), not MWo3 id order.
set(DQ8_OVERLAY_NAMES title casino viewer battle menu shop)

separate_arguments(DQ8_OVERLAY_OPTFLAGS_LIST UNIX_COMMAND "${DQ8_GENERATED_OPTFLAGS}")

set(DQ8_OVERLAY_LIBS "")
foreach(overlay IN LISTS DQ8_OVERLAY_NAMES)
    set(overlay_dir "${DQ8_OVERLAY_GENERATED_DIR}/${overlay}")

    file(GLOB overlay_sources CONFIGURE_DEPENDS "${overlay_dir}/*.cpp")
    # ps2_recomp's own registration file defines the program-wide function
    # table symbols; tools/mwo3/gen_overlay_table.py has already re-emitted its
    # contents as overlay_table_<name>.cpp inside namespace ovl_<name>.
    list(REMOVE_ITEM overlay_sources "${overlay_dir}/register_functions.cpp")

    list(LENGTH overlay_sources overlay_source_count)
    if(overlay_source_count EQUAL 0)
        message(FATAL_ERROR
            "DQ8_LINK_OVERLAYS=ON but no translated sources under ${overlay_dir}. "
            "Run ps2_recomp over config/SLUS_212.07/overlays/${overlay}.toml, then "
            "tools/mwo3/gen_overlay_table.py.")
    endif()
    if(NOT EXISTS "${overlay_dir}/overlay_table_${overlay}.cpp")
        message(FATAL_ERROR
            "Missing ${overlay_dir}/overlay_table_${overlay}.cpp. Generate it with: "
            "python3 tools/mwo3/gen_overlay_table.py --all ${DQ8_OVERLAY_GENERATED_DIR}")
    endif()

    add_library(dq8_ovl_${overlay} STATIC ${overlay_sources})
    target_link_libraries(dq8_ovl_${overlay} PRIVATE dq8_host_simd)

    # The overlay's own generated headers must come first: every overlay tree
    # has a ps2_recompiled_functions.h and they all share one include guard.
    target_include_directories(dq8_ovl_${overlay} PRIVATE
        "${overlay_dir}"
        "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/include"
        "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/include/runtime"
        "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xIOP/include"
        "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/src/lib/Kernel")

    target_compile_options(dq8_ovl_${overlay} PRIVATE ${DQ8_OVERLAY_OPTFLAGS_LIST})
    target_precompile_headers(dq8_ovl_${overlay} PRIVATE
        <ps2_runtime_macros.h> <ps2_runtime.h>
        <ps2_recompiled_functions.h> <ps2_recompiled_stubs.h>
        <ps2_syscalls.h> <ps2_stubs.h>)

    message(STATUS "dq8: overlay ${overlay}: ${overlay_source_count} translation units")
    list(APPEND DQ8_OVERLAY_LIBS dq8_ovl_${overlay})
endforeach()

add_library(dq8_overlay STATIC overlay_manager.cpp)
target_link_libraries(dq8_overlay PRIVATE dq8_host_simd)
target_include_directories(dq8_overlay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(dq8_overlay PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/include/runtime"
    "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xIOP/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/PS2Recomp/ps2xRuntime/src/lib/Kernel")

# The manager names ovl_<name>::functionRegion() for all six, so every overlay
# archive is pulled in by that reference alone.
target_link_libraries(dq8_overlay PUBLIC ${DQ8_OVERLAY_LIBS})
