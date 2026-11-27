# The line a compiled shader's header carries to say which GLSL it came from, so
# a header compiled ahead (DQ8_PREBUILT_SHADERS) is never used for another
# source. Carriage returns are left out: a checkout's line endings do not count.
function(dq8_shader_stamp OUT SOURCE)
    file(READ "${SOURCE}" TEXT)
    string(REPLACE "\r" "" TEXT "${TEXT}")
    string(SHA256 HASH "${TEXT}")
    set(${OUT} "// source-sha256: ${HASH}" PARENT_SCOPE)
endfunction()
