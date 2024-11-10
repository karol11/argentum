add_library (ag_runtime STATIC
    runtime.h
    runtime.c
    blob.h
    blob.c
    ag-threads.h
    ag-queue.h
    ag-queue.c
    utf8.h
    utf8.c
    array/array-base-inc.h
    array/array-base.h
    array/array-base.c
    array/own-array.h
    array/own-array.c
    array/weak-array.h
    array/weak-array.c
    array/shared-array.h
    array/shared-array.c
    map/map-base.h
    map/map-base.c
    map/own-map.h
    map/own-map.c
    map/shared-map.h
    map/shared-map.c
    map/weak-map.h
    map/weak-map.c
)
target_compile_definitions(ag_runtime PRIVATE AG_STANDALONE_COMPILER_MODE)
set_property(TARGET ag_runtime PROPERTY C_STANDARD 11)
target_include_directories(ag_runtime  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
