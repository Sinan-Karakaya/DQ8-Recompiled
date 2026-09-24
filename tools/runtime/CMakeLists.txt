cmake_minimum_required(VERSION 3.24)
project(dq8_runtime_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
get_filename_component(DQ8_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PS2X "${DQ8_ROOT}/thirdparty/PS2Recomp")
enable_testing()
set(FIBER_VARIANTS native)
# Darwin's deprecated ucontext implementation does not support ARM64 stacks.
if(NOT APPLE)
    list(APPEND FIBER_VARIANTS ucontext)
endif()
foreach(variant IN LISTS FIBER_VARIANTS)
    add_executable(dq8-fiber-${variant}
        fiber_main.cpp
        "${PS2X}/ps2xTest/src/ee_fiber_tests.cpp"
        "${PS2X}/ps2xRuntime/src/lib/Kernel/EeFiber.cpp")
    target_include_directories(dq8-fiber-${variant} PRIVATE
        "${PS2X}/ps2xTest/include" "${PS2X}/ps2xRuntime/include")
    target_compile_options(dq8-fiber-${variant} PRIVATE -O2)
    if(variant STREQUAL "ucontext")
        target_compile_definitions(dq8-fiber-${variant} PRIVATE EE_FIBER_FORCE_UCONTEXT=1)
    endif()
    add_test(NAME fiber-${variant} COMMAND dq8-fiber-${variant})
endforeach()

if(TARGET ps2_runtime)
    add_executable(dq8-render-cadence-tests render_cadence_main.cpp
        "${PS2X}/ps2xTest/src/test_function_table.cpp")
    target_include_directories(dq8-render-cadence-tests PRIVATE "${PS2X}/ps2xTest/include")
    target_link_libraries(dq8-render-cadence-tests PRIVATE ps2_runtime dq8_host_simd)
    set(cadence_generated "${DQ8_GENERATED_DIR}/FUN_00145080_0x145080.cpp")
    if(EXISTS "${cadence_generated}")
        target_sources(dq8-render-cadence-tests PRIVATE "${cadence_generated}")
        target_include_directories(dq8-render-cadence-tests PRIVATE "${DQ8_GENERATED_DIR}")
        target_compile_definitions(dq8-render-cadence-tests PRIVATE DQ8_CADENCE_GENERATED_SMOKE=1)
    endif()
    add_test(NAME render-cadence COMMAND dq8-render-cadence-tests)
    set_tests_properties(render-cadence PROPERTIES TIMEOUT 60)

    add_executable(dq8-kernel-tests
        kernel_main.cpp
        "${PS2X}/ps2xTest/src/test_function_table.cpp"
        "${PS2X}/ps2xTest/src/ps2_runtime_kernel_tests.cpp"
        "${PS2X}/ps2xTest/src/ps2_runtime_io_tests.cpp"
        "${PS2X}/ps2xTest/src/ps2_sif_rpc_tests.cpp"
        "${PS2X}/ps2xTest/src/ps2_runtime_interrupt_tests.cpp")
    target_include_directories(dq8-kernel-tests PRIVATE
        "${PS2X}/ps2xTest/include" "${PS2X}/ps2xRuntime/src/lib")
    target_link_libraries(dq8-kernel-tests PRIVATE ps2_runtime dq8_host_simd)
    add_test(NAME kernel COMMAND dq8-kernel-tests)
    set_tests_properties(kernel PROPERTIES TIMEOUT 60)

    add_executable(dq8-ee-pacing-tests
        "${PS2X}/ps2xTest/src/ps2_ee_pacing_tests.cpp"
        "${PS2X}/ps2xTest/src/test_function_table.cpp")
    target_include_directories(dq8-ee-pacing-tests PRIVATE
        "${PS2X}/ps2xTest/include" "${PS2X}/ps2xRuntime/src/lib/Kernel")
    target_link_libraries(dq8-ee-pacing-tests PRIVATE ps2_runtime dq8_host_simd)
    add_test(NAME ee-pacing COMMAND dq8-ee-pacing-tests)
    set_tests_properties(ee-pacing PROPERTIES TIMEOUT 60)

    add_executable(dq8-vu-tests
        vu_main.cpp
        "${PS2X}/ps2xTest/src/test_function_table.cpp"
        "${PS2X}/ps2xTest/src/ps2_vu1_tests.cpp"
        "${PS2X}/ps2xTest/src/ps2_vu1_upper_engine_tests.cpp"
        "${PS2X}/ps2xTest/src/ps2_vu1_program_tests.cpp")
    target_include_directories(dq8-vu-tests PRIVATE "${PS2X}/ps2xTest/include")
    target_link_libraries(dq8-vu-tests PRIVATE ps2_runtime dq8_host_simd)
    add_test(NAME vu COMMAND dq8-vu-tests)
    # Differential cases repeatedly initialize full PS2 memory on hosted CPUs.
    set_tests_properties(vu PROPERTIES TIMEOUT 600)

    add_executable(dq8-vu-math-tests
        vu_math_main.cpp
        "${PS2X}/ps2xTest/src/test_function_table.cpp"
        "${PS2X}/ps2xTest/src/ps2_vu_tests.cpp")
    target_include_directories(dq8-vu-math-tests PRIVATE
        "${PS2X}/ps2xTest/include" "${PS2X}/ps2xRuntime/src/lib")
    target_link_libraries(dq8-vu-math-tests PRIVATE ps2_runtime dq8_host_simd)
    add_test(NAME vu-math COMMAND dq8-vu-math-tests)
    set_tests_properties(vu-math PROPERTIES TIMEOUT 60)

    add_executable(dq8-vu-bounds-tests vu_bounds_main.cpp
        "${DQ8_ROOT}/src/runtime/vu_bounds.cpp"
        "${PS2X}/ps2xTest/src/test_function_table.cpp")
    target_include_directories(dq8-vu-bounds-tests PRIVATE
        "${PS2X}/ps2xTest/include" "${DQ8_ROOT}/src/runtime")
    target_link_libraries(dq8-vu-bounds-tests PRIVATE ps2_runtime dq8_host_simd)
    add_test(NAME vu-bounds COMMAND dq8-vu-bounds-tests)
    set_tests_properties(vu-bounds PROPERTIES TIMEOUT 60)

    add_executable(dq8-vu-replay
        vu_replay.cpp "${PS2X}/ps2xTest/src/test_function_table.cpp")
    target_link_libraries(dq8-vu-replay PRIVATE ps2_runtime dq8_host_simd)

    add_executable(dq8-memory-tests
        memory_main.cpp
        "${PS2X}/ps2xTest/src/test_function_table.cpp"
        "${PS2X}/ps2xTest/src/ps2_memory_tests.cpp")
    target_include_directories(dq8-memory-tests PRIVATE "${PS2X}/ps2xTest/include")
    target_link_libraries(dq8-memory-tests PRIVATE ps2_runtime dq8_host_simd)
    add_test(NAME memory COMMAND dq8-memory-tests)
    set_tests_properties(memory PROPERTIES TIMEOUT 60)
endif()
