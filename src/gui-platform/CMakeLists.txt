find_package(unofficial-skia CONFIG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

add_library (gui_platform STATIC
    gui-platform.cpp
	skia-bind.cpp
	skia-bind.h
)
set_property(TARGET gui_platform PROPERTY C_STANDARD 11)

target_link_libraries(gui_platform PRIVATE OpenGL::GL)
target_link_libraries(gui_platform PRIVATE unofficial::skia::skia)
target_link_libraries(gui_platform
	PRIVATE
	$<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
	$<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

target_link_libraries(gui_platform PRIVATE ag_runtime)

#ag_copy_win_dll(ag_sdl)
#ag_copy_libs(gui_platform OpenGL::GL)
#ag_copy_libs(gui_platform unofficial::skia::skia)
ag_copy_libs(gui_platform SDL2::SDL2)
