# Argentum
cmake_minimum_required (VERSION 3.8)

project ("codegen")

set(CMAKE_CXX_STANDARD 17)
add_definitions(-D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

set(ag_sources
    src/utils/utf8.h
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

    src/compiler/generator.h
    src/compiler/generator12.cpp
#    src/compiler/generator15.cpp

    src/utils/vmt_util.h

    src/utils/runtime.h
    src/utils/runtime.cpp
)

add_executable(ag-jit-sdl
    ${ag_sources}
    src/driver/jit-sdl.cpp
)
target_link_libraries(ag-jit-sdl
    ${SDL2_LIBRARIES}
    ${SDL2_IMAGE_LIBRARIES}
    ${llvm_libs})

add_executable(ag-tests
    ${ag_sources}
    src/utils/fake-gunit.h
    src/utils/fake-gunit.cpp
    src/ltm/ltm-test.cpp
    src/dom/dom-test.cpp
    src/compiler/compiler-test.cpp
    src/utils/vmt_util-test.cpp
)
target_link_libraries(ag-tests ${llvm_libs})

add_executable(agc
    ${ag_sources}
    src/driver/compiler.cpp
)
target_link_libraries(agc ${llvm_libs})
