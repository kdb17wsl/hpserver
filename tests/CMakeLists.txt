find_package(GTest REQUIRED)

enable_testing()

add_executable(http_conn_test
    http_conn_test.cpp
    ../src/net/http/http_conn.cpp
    ../src/net/http/http_connection_io.cpp
    ../src/net/http/http_request_parser.cpp
    ../src/net/proxy/http_proxy.cpp
    ../src/net/core/poller.cpp
    ../src/net/core/socket_ops.cpp
)

target_link_libraries(http_conn_test
    PRIVATE
        hpserver_headers
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
