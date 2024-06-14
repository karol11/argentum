project("ag_sqlite")

find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library (ag_sqlite STATIC sqlite-ffi.c)

set_property(TARGET ag_sqlite PROPERTY C_STANDARD 11)
target_link_libraries(ag_sqlite PRIVATE ag_runtime)
target_link_libraries(ag_sqlite PRIVATE unofficial::sqlite3::sqlite3)

ag_copy_libs(ag_sqlite unofficial::sqlite3::sqlite3)
ag_copy_ag(ag_sqlite "sqliteFfi.ag")
