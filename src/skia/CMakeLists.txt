find_package(unofficial-skia CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_library(skia_bind STATIC skia-bind.cpp)
set_property(TARGET skia_bind PROPERTY C_STANDARD 11)

target_link_libraries(skia_bind PRIVATE ag_runtime)
target_link_libraries(skia_bind PRIVATE unofficial::skia::skia)
