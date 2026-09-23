# Wraps a SPIR-V module as a constexpr uint32_t array in a generated header.
# Run in script mode by dq8_add_shader; expects INPUT, OUTPUT and SYMBOL.

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "Dq8EmbedSpirv.cmake requires INPUT, OUTPUT and SYMBOL")
endif()

file(SIZE "${INPUT}" SPIRV_SIZE)
if(SPIRV_SIZE EQUAL 0)
    message(FATAL_ERROR "Dq8EmbedSpirv.cmake: ${INPUT} is empty")
endif()
math(EXPR SPIRV_REMAINDER "${SPIRV_SIZE} % 4")
if(NOT SPIRV_REMAINDER EQUAL 0)
    message(FATAL_ERROR "Dq8EmbedSpirv.cmake: ${INPUT} is ${SPIRV_SIZE} bytes, not a whole number of SPIR-V words")
endif()

file(READ "${INPUT}" SPIRV_HEX HEX)
string(REGEX MATCHALL "[0-9a-f][0-9a-f]" SPIRV_BYTES "${SPIRV_HEX}")

# SPIR-V is a little-endian word stream; emit words rather than bytes so the
# array is correctly aligned for the API without a memcpy at load time.
set(WORDS "")
list(LENGTH SPIRV_BYTES BYTE_COUNT)
math(EXPR LAST_WORD "${BYTE_COUNT} / 4 - 1")
foreach(INDEX RANGE ${LAST_WORD})
    math(EXPR B0 "${INDEX} * 4")
    math(EXPR B1 "${B0} + 1")
    math(EXPR B2 "${B0} + 2")
    math(EXPR B3 "${B0} + 3")
    list(GET SPIRV_BYTES ${B0} BYTE0)
    list(GET SPIRV_BYTES ${B1} BYTE1)
    list(GET SPIRV_BYTES ${B2} BYTE2)
    list(GET SPIRV_BYTES ${B3} BYTE3)
    string(APPEND WORDS "0x${BYTE3}${BYTE2}${BYTE1}${BYTE0}u,")
    math(EXPR NEWLINE "${INDEX} % 8")
    if(NEWLINE EQUAL 7)
        string(APPEND WORDS "\n    ")
    else()
        string(APPEND WORDS " ")
    endif()
endforeach()

get_filename_component(INPUT_NAME "${INPUT}" NAME)
set(CONTENT "// Generated from ${INPUT_NAME}. Do not edit.\n")
string(APPEND CONTENT "#pragma once\n\n#include <cstdint>\n\n")
string(APPEND CONTENT "inline constexpr uint32_t ${SYMBOL}[] = {\n    ${WORDS}\n};\n")
file(WRITE "${OUTPUT}" "${CONTENT}")
