# Embed generated shader source, including its terminating NUL.
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "Dq8EmbedText requires INPUT, OUTPUT and SYMBOL")
endif()
file(READ "${INPUT}" SOURCE)
if(SOURCE STREQUAL "" OR SOURCE MATCHES "\\)DQ8_SHADER\"")
    message(FATAL_ERROR "Invalid shader source: ${INPUT}")
endif()
file(WRITE "${OUTPUT}"
    "// Generated shader source. Do not edit.\n#pragma once\ninline constexpr char ${SYMBOL}[] = R\"DQ8_SHADER(${SOURCE})DQ8_SHADER\";\n")
