# Before runtime: dq8 links the SDL GPU backend and its menu when they are
# available, so the targets have to exist first.
add_subdirectory(gfx)
add_subdirectory(ui)
add_subdirectory(runtime)
