cmake_minimum_required(VERSION 3.14)

project(hpserver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hpserver_headers INTERFACE)
target_include_directories(hpserver_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/core
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/http
)

add_library(llhttp STATIC
  third_party/llhttp/src/llhttp.c
  third_party/llhttp/src/api.c
  third_party/llhttp/src/http.c
)

target_include_directories(llhttp PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llhttp/include
)

target_compile_features(llhttp PRIVATE c_std_99)

target_link_libraries(hpserver_headers INTERFACE llhttp)

add_executable(hpserver
  src/main.cpp
  src/hpserver.cpp
  src/net/core/poller.cpp
  src/net/core/socket_ops.cpp
  src/net/http/http_conn.cpp
  src/net/http/http_connection_io.cpp
  src/net/http/http_request_parser.cpp
  src/net/http/http_test_response.cpp
  src/net/proxy/http_proxy.cpp
  src/net/proxy/relay_engine.cpp
  src/net/proxy/upstream_connector.cpp
)

target_link_libraries(hpserver
  PRIVATE
    hpserver_headers
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
