# Embed generated shader source, including its terminating NUL. Takes
# SHADER_SOURCE to stamp the header with the GLSL it came from.
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "Dq8EmbedText requires INPUT, OUTPUT and SYMBOL")
endif()
file(READ "${INPUT}" SOURCE)
if(SOURCE STREQUAL "" OR SOURCE MATCHES "\\)DQ8_SHADER\"")
    message(FATAL_ERROR "Invalid shader source: ${INPUT}")
endif()
set(STAMP_LINE "")
if(DEFINED SHADER_SOURCE)
    include("${CMAKE_CURRENT_LIST_DIR}/Dq8ShaderStamp.cmake")
    dq8_shader_stamp(STAMP "${SHADER_SOURCE}")
    set(STAMP_LINE "${STAMP}\n")
endif()
file(WRITE "${OUTPUT}"
    "// Generated shader source. Do not edit.\n${STAMP_LINE}#pragma once\ninline constexpr char ${SYMBOL}[] = R\"DQ8_SHADER(${SOURCE})DQ8_SHADER\";\n")
