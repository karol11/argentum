cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  MESSAGE(FATAL_ERROR "Toolchain file not set, try using: cmake --preset default build")
endif()

project (Argentum)

set(OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:/debug>/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:/debug>/libs)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:/debug>/workdir)

function(ag_copy_prop target dep_target property dst_dir)
    get_property(val TARGET ${dep_target} PROPERTY ${property})
    if(val)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${val} ${OUTPUT_DIRECTORY}/${dst_dir}
        )
    endif()
endfunction()

function(ag_copy_libs target dep_target)
    ag_copy_prop(${target} ${dep_target} "IMPORTED_IMPLIB_DEBUG" "debug/libs")
    ag_copy_prop(${target} ${dep_target} "IMPORTED_LOCATION_DEBUG" "debug/workdir")
    ag_copy_prop(${target} ${dep_target} "IMPORTED_IMPLIB_RELEASE" "libs")
    ag_copy_prop(${target} ${dep_target} "IMPORTED_LOCATION_RELEASE" "workdir")
endfunction()

function(ag_copy_ag target file)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/${file} ${OUTPUT_DIRECTORY}/ag
    )
endfunction()

function(print_target_properties target_name)
    message(STATUS "Properties of target: ${target_name}")
    set(properties
        NAME
        ALIASED_TARGET
        SOURCES
        INCLUDE_DIRECTORIES
        COMPILE_OPTIONS
        COMPILE_DEFINITIONS
        COMPILE_FEATURES
        LINK_LIBRARIES
        LINK_OPTIONS
        LINK_DIRECTORIES
        LINK_DEPENDS
        IMPORTED_IMPLIB_DEBUG
        IMPORTED_IMPLIB_RELEASE
        IMPORTED_LOCATION_DEBUG
        IMPORTED_LOCATION_RELEASE
        LOCATION
        ARCHIVE_OUTPUT_DIRECTORY
        LIBRARY_OUTPUT_DIRECTORY
        RUNTIME_OUTPUT_DIRECTORY
        IMPORTED_IMPLIB
        IMPORTED_LOCATION
        IMPORTED
        INTERFACE_INCLUDE_DIRECTORIES
        INTERFACE_COMPILE_DEFINITIONS
        INTERFACE_COMPILE_OPTIONS
        INTERFACE_LINK_LIBRARIES
        INTERFACE_LINK_OPTIONS
        INSTALL_RPATH
        INSTALL_RPATH_USE_LINK_PATH
        VERSION
        SOVERSION
        COMPATIBLE_INTERFACE_STRING
        COMPATIBLE_INTERFACE_BOOL
        COMPATIBLE_INTERFACE_NUMBER_MAX
        COMPATIBLE_INTERFACE_NUMBER_MIN
        POSITION_INDEPENDENT_CODE
        CUDA_PTX_COMPILATION
        CXX_STANDARD
        CXX_EXTENSIONS
        CXX_STANDARD_REQUIRED
    )
    foreach(property ${properties})
        get_target_property(value ${target_name} ${property})
        if(NOT value)
            set(value "NOT SET")
        endif()
        message(STATUS "  ${property} = ${value}")
    endforeach()
endfunction()

add_subdirectory("src/runtime")
add_subdirectory("src/compiler")
add_subdirectory("src/sqlite")
add_subdirectory("src/http-client")
add_subdirectory("src/sdl")
