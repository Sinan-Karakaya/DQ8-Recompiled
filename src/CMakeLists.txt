# Before runtime: dq8 links the SDL GPU backend when it is available, so the
# target has to exist first.
add_subdirectory(gfx)
add_subdirectory(runtime)
