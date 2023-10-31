# Argentum
cmake_minimum_required (VERSION 3.8)
set(CMAKE_CXX_STANDARD 17)
add_definitions(-D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project ("argentum")

find_package(LLVM REQUIRED CONFIG PATHS ../llvm12.0.1r/lib/cmake/llvm NO_DEFAULT_PATH)
#find_package(LLVM REQUIRED CONFIG PATHS ../llvm-15-lib/lib/cmake/llvm NO_DEFAULT_PATH)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(llvm_libs support core nativecodegen orcjit)

find_package(SDL2 REQUIRED)
find_package(SDL2_image REQUIRED)
include_directories(${CMAKE_CURRENT_LIST_DIR}/src ${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS})
set(CMAKE_CXX_FLAGS_RELEASE "/MT")

set(ag_rtl_sources
    src/runtime/runtime.h
    src/runtime/runtime.c
    src/runtime/blob.h
    src/runtime/blob.c
    src/runtime/array/array-base.inc
    src/runtime/array/own-array.h
    src/runtime/array/own-array.c
    src/runtime/array/weak-array.h
    src/runtime/array/weak-array.c
    src/runtime/array/shared-array.h
    src/runtime/array/shared-array.c
    src/runtime/map/map-base.h
    src/runtime/map/map-base.c
    src/runtime/map/own-map.h
    src/runtime/map/own-map.c
    src/runtime/map/shared-map.h
    src/runtime/map/shared-map.c
    src/runtime/map/weak-map.h
    src/runtime/map/weak-map.c
    src/utils/utf8.h
    src/utils/utf8.c
)
set(ag_sources
    src/utils/utf8.h
    src/utils/utf8.c
    src/ltm/ltm.h
    src/ltm/ltm.cpp

    src/dom/dom.h
    src/dom/dom.cpp

    src/dom/dom-to-string.cpp
    src/dom/dom-to-string.h

    src/compiler/ast.h
    src/compiler/ast.cpp

    src/compiler/parser.h
    src/compiler/parser.cpp

    src/compiler/name-resolver.h
    src/compiler/name-resolver.cpp

    src/compiler/type-checker.h
    src/compiler/type-checker.cpp

    src/compiler/const-capture-pass.h
    src/compiler/const-capture-pass.cpp

    src/compiler/generator.h
    src/compiler/generator.cpp

    src/utils/register_runtime.h
    src/utils/register_runtime.cpp

    src/utils/vmt_util.h
)

add_executable(ag-jit-sdl
    ${ag_sources}
    src/driver/jit-sdl.cpp
    ${ag_rtl_sources}
    src/runtime/sdl-bindings.h
    src/runtime/sdl-bindings.c
)
target_link_libraries(ag-jit-sdl
    ${SDL2_LIBRARIES}
    ${SDL2_IMAGE_LIBRARIES}
    ${llvm_libs}
)

add_executable(ag-tests
    ${ag_sources}
    ${ag_rtl_sources}
    src/utils/fake-gunit.h
    src/utils/fake-gunit.cpp
    src/ltm/ltm-test.cpp
    src/dom/dom-test.cpp
    src/compiler/compiler-test.cpp
    src/utils/vmt_util-test.cpp
    src/runtime/map/own-map-test.cpp
)
target_link_libraries(ag-tests ${llvm_libs})

add_executable(agc
    ${ag_sources}
    src/driver/compiler.cpp
)
target_compile_definitions(agc PRIVATE AG_STANDALONE_COMPILER_MODE)
target_link_libraries(agc ${llvm_libs})

add_library (ag_runtime STATIC
    ${ag_rtl_sources}
    src/runtime/sdl-bindings.h
    src/runtime/sdl-bindings.c
)
set_property(TARGET ag_runtime PROPERTY C_STANDARD 11)
