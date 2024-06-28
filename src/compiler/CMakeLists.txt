project ("agc")

set(CMAKE_CXX_STANDARD 17)
add_definitions(-D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)

find_package(LLVM CONFIG REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(HandleLLVMOptions)
add_definitions(${LLVM_DEFINITIONS})

add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(llvm_libs ${LLVM_TARGETS_TO_BUILD})

add_executable(agc
    utils/utf8.h
    utils/utf8.c
    ltm/ltm.h
    ltm/ltm.cpp

    dom/dom.h
    dom/dom.cpp

    dom/dom-to-string.cpp
    dom/dom-to-string.h

    ast.h
    ast.cpp

    parser.h
    parser.cpp

    name-resolver.h
    name-resolver.cpp

    type-checker.h
    type-checker.cpp

    pruner.h
    pruner.cpp

    const-capture-pass.h
    const-capture-pass.cpp

    generator.h
    generator.cpp

    utils/register_runtime.h
    utils/register_runtime.cpp

    utils/vmt_util.h
    compiler.cpp
)
target_compile_definitions(agc PRIVATE AG_STANDALONE_COMPILER_MODE)
target_include_directories(agc PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(agc  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(agc PRIVATE ${llvm_libs})
target_link_libraries(agc PRIVATE ag_runtime) #TODO:remove after reg-runtime redesign

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(agc PRIVATE "/EHsc")
endif()
