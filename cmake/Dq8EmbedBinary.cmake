# Wraps any file as a byte array in a generated C++ source.
# Run in script mode; expects INPUT, OUTPUT and SYMBOL. The array is defined
# with external linkage, as `extern const unsigned char SYMBOL[]` plus
# `extern const unsigned int SYMBOL_size`.

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "Dq8EmbedBinary.cmake requires INPUT, OUTPUT and SYMBOL")
endif()

file(READ "${INPUT}" HEX HEX)
string(LENGTH "${HEX}" HEX_LENGTH)
if(HEX_LENGTH EQUAL 0)
    message(FATAL_ERROR "Dq8EmbedBinary.cmake: ${INPUT} is empty")
endif()
math(EXPR SIZE "${HEX_LENGTH} / 2")

# 32 bytes a line, in chunks: one regex over the whole file is fine, but a
# per-byte loop is quadratic in CMake.
set(BODY "")
set(OFFSET 0)
while(OFFSET LESS HEX_LENGTH)
    string(SUBSTRING "${HEX}" ${OFFSET} 64 CHUNK)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CHUNK "${CHUNK}")
    string(APPEND BODY "    ${CHUNK}\n")
    math(EXPR OFFSET "${OFFSET} + 64")
endwhile()

get_filename_component(INPUT_NAME "${INPUT}" NAME)
file(WRITE "${OUTPUT}"
    "// Generated from ${INPUT_NAME}. Do not edit.\n"
    "extern const unsigned char ${SYMBOL}[] = {\n${BODY}};\n"
    "extern const unsigned int ${SYMBOL}_size = ${SIZE}u;\n")
