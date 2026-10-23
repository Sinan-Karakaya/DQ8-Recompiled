include_guard(GLOBAL)
add_library(dq8_host_simd INTERFACE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    include(FetchContent)
    FetchContent_Declare(sse2neon
        GIT_REPOSITORY https://github.com/DLTcollab/sse2neon.git
        GIT_TAG v1.9.1 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(sse2neon)
    target_include_directories(dq8_host_simd SYSTEM INTERFACE "${sse2neon_SOURCE_DIR}")
    target_compile_definitions(dq8_host_simd INTERFACE USE_SSE2NEON)
elseif(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    # ps2_runtime.h's inline helpers use SSE4.1 intrinsics, which an optimizing
    # GCC will not inline into code built for the SSE2 baseline.
    target_compile_options(dq8_host_simd INTERFACE -msse4.1)
endif()
