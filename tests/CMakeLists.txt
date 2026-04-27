find_package(GTest REQUIRED)

enable_testing()

include(GoogleTest)

function(add_hpserver_gtest target_name source_file)
    add_executable(${target_name}
        ${source_file}
    )

    target_link_libraries(${target_name}
        PRIVATE
            ${ARGN}
            GTest::gtest_main
    )

    gtest_discover_tests(${target_name})
endfunction()

add_hpserver_gtest(http_conn_test http_conn_test.cpp hpserver_lib)
add_hpserver_gtest(http_response_parser_test http_response_parser_test.cpp hpserver_lib)
add_hpserver_gtest(threadsafe_queue_test threadsafe_queue_test.cpp hpserver_headers)
add_hpserver_gtest(thread_pool_test thread_pool_test.cpp hpserver_headers)
add_hpserver_gtest(bloom_filter_test bloom_filter_test.cpp hpserver_headers)
add_hpserver_gtest(bitwise_trie_test bitwise_trie_test.cpp hpserver_headers)
add_hpserver_gtest(ip_filter_test ip_filter_test.cpp hpserver_lib)
add_hpserver_gtest(memory_cache_test memory_cache_test.cpp hpserver_headers)
