# The project's icon (res/icon, drawn by make_icon.py) on an executable:
# Windows shows the one in its resources, macOS the one in its app bundle.
# Linux executables carry none, so there the window sets its own.

include_guard(GLOBAL)

function(dq8_add_icon target)
    set(icons "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../res/icon")
    if(WIN32)
        # rc.exe and windres find dq8.ico beside the .rc file.
        target_sources(${target} PRIVATE "${icons}/dq8.rc")
    elseif(APPLE)
        get_target_property(bundle ${target} MACOSX_BUNDLE)
        if(bundle)
            target_sources(${target} PRIVATE "${icons}/dq8.icns")
            set_source_files_properties("${icons}/dq8.icns" TARGET_DIRECTORY ${target}
                PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
            set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE dq8.icns)
        endif()
    endif()
endfunction()

# The window icon, as the kWindowIconBmp array sdlgpu_window_icon.cpp hands to
# SDL_SetWindowIcon. Only Linux needs it: SDL takes Windows' from the
# executable, and macOS shows the bundle's.
function(dq8_embed_window_icon target)
    if(WIN32 OR APPLE)
        return()
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}_window_icon.cpp")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../res/icon/dq8-window.bmp"
                "-DOUTPUT=${output}" -DSYMBOL=kWindowIconBmp
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Dq8EmbedBinary.cmake"
        DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../res/icon/dq8-window.bmp"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Dq8EmbedBinary.cmake"
        COMMENT "Embedding the window icon"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()
