# Given to PS2Recomp's configure as CMAKE_PROJECT_PS2Recomp_INCLUDE, which runs
# at the end of ps2xRecomp's project() call: what the recompiler needs here,
# with the submodule unchanged.

include("${CMAKE_CURRENT_LIST_DIR}/Dq8Icon.cmake")

function(dq8_adjust_recompiler)
    # The project's icon, like the launcher's and the game's.
    dq8_add_icon(ps2_recomp)
    # code_generator.h holds an unordered_map of Symbol, which it only
    # declares; libstdc++ before 13 refuses that, and releases build Linux
    # on Ubuntu 22.04's GCC 11 for its older glibc. types.h defines Symbol.
    if(NOT MSVC)
        foreach(target ps2_recomp_lib ps2_recomp)
            target_compile_options(${target} PRIVATE "SHELL:-include ps2recomp/types.h")
        endforeach()
    endif()
endfunction()
# The targets are declared further down that file; this runs once it is done.
cmake_language(DEFER CALL dq8_adjust_recompiler)
