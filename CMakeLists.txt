cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  MESSAGE(FATAL_ERROR "Toolchain file not set, try using: cmake --preset default")
endif()

project (Argentum)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:/debug>/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:/debug>/libs)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:/debug>/apps)

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
    ag_copy_prop(${target} ${dep_target} "IMPORTED_IMPLIB_RELEASE" "libs")
    ag_copy_prop(${target} ${dep_target} "IMPORTED_LOCATION_RELEASE" "apps")
endfunction()

function(ag_copy_ag target file)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/${file} ${OUTPUT_DIRECTORY}/ag
    )
endfunction()

function(ag_copy_win_dll target)
    if (WIN32)
        foreach(dll_name ${ARGN})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${OUTPUT_DIRECTORY}/../build/vcpkg_installed/x64-windows/bin/${dll_name}.dll
                    ${OUTPUT_DIRECTORY}/apps
            )
        endforeach()
    endif()
endfunction()

add_subdirectory("src/runtime")
add_subdirectory("src/compiler")
add_subdirectory("src/sqlite")
add_subdirectory("src/http-client")
add_subdirectory("src/sdl")
add_subdirectory("src/gui-platform")
