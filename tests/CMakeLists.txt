find_package(GTest REQUIRED)

enable_testing()

add_executable(http_conn_test
    http_conn_test.cpp
)

target_link_libraries(http_conn_test
    PRIVATE
        hpserver_lib
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(http_conn_test)

add_executable(threadsafe_queue_test
    threadsafe_queue_test.cpp
)

target_link_libraries(threadsafe_queue_test
    PRIVATE
        hpserver_headers
        GTest::gtest_main
)

gtest_discover_tests(threadsafe_queue_test)

add_executable(thread_pool_test
    thread_pool_test.cpp
)

target_link_libraries(thread_pool_test
    PRIVATE
        hpserver_headers
        GTest::gtest_main
)

gtest_discover_tests(thread_pool_test)

add_executable(bloom_filter_test
    bloom_filter_test.cpp
)

target_link_libraries(bloom_filter_test
    PRIVATE
        hpserver_headers
        GTest::gtest_main
)

gtest_discover_tests(bloom_filter_test)

add_executable(bitwise_trie_test
    bitwise_trie_test.cpp
)

target_link_libraries(bitwise_trie_test
    PRIVATE
        hpserver_headers
        GTest::gtest_main
)

gtest_discover_tests(bitwise_trie_test)
