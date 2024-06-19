project(ag_http_client)

find_package(CURL REQUIRED)

add_library (ag_http_client STATIC http-client.c)

set_property(TARGET ag_http_client PROPERTY C_STANDARD 11)
target_link_libraries(ag_http_client PRIVATE ag_runtime)

if(TARGET CURL::libcurl_shared)
    target_link_libraries(ag_http_client PRIVATE CURL::libcurl_shared)
    ag_copy_libs(ag_http_client CURL::libcurl_shared)
else()
    target_link_libraries(ag_http_client PRIVATE CURL::libcurl)
    ag_copy_libs(ag_http_client CURL::libcurl)
endif()
ag_copy_win_dll(ag_http_client zlib1)

ag_copy_ag(ag_http_client "httpClient.ag")
