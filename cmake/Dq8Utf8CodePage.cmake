# Windows reads argv, and every path given to fopen, std::ifstream or a
# std::filesystem::path made from a std::string, in the ANSI code page. SDL
# hands paths over as UTF-8, so under a user folder such as C:\Users\João the
# settings file never opened, and a name outside that code page reached argv
# as question marks. Since Windows 10 1903 this manifest makes the process
# code page UTF-8.

include_guard(GLOBAL)

function(dq8_use_utf8_code_page target)
    if(MSVC)
        # Merged into the manifest the linker writes.
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.manifest")
    elseif(MINGW)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.rc")
    endif()
endfunction()
