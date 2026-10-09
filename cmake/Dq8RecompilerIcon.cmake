# Given to PS2Recomp's configure as CMAKE_PROJECT_PS2Recomp_INCLUDE, which runs
# at the end of ps2xRecomp's project() call: the recompiler then carries the
# project's icon like the launcher and the game, with the submodule unchanged.

include("${CMAKE_CURRENT_LIST_DIR}/Dq8Icon.cmake")
# ps2_recomp is declared further down that file; this runs once it is done.
cmake_language(DEFER CALL dq8_add_icon ps2_recomp)
