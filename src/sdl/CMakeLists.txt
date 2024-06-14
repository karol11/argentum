project("ag_sdl")

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_image CONFIG REQUIRED)

add_library (ag_sdl STATIC
    sdl-bindings.c
    sdl-bindings.h
)
set_property(TARGET ag_sdl PROPERTY C_STANDARD 11)
target_include_directories(ag_sdl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ag_sdl PRIVATE ag_runtime)
target_link_libraries(ag_sdl PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)
target_link_libraries(ag_sdl PRIVATE
    $<IF:$<TARGET_EXISTS:SDL2_image::SDL2_image>,SDL2_image::SDL2_image,SDL2_image::SDL2_image-static>
)
if(TARGET SDL2_image::SDL2_image)
    ag_copy_libs(ag_sdl SDL2_image::SDL2_image)
else()
    ag_copy_libs(ag_sdl SDL2_image::SDL2_image-static)
endif()
ag_copy_libs(ag_sdl SDL2::SDL2)
ag_copy_ag(ag_sdl "sdl.ag")
