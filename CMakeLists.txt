cmake_minimum_required(VERSION 3.14)

project(hpserver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HPSERVER_LINK_ROCKSDB_STATIC_DEPS "Link common static dependency libs needed by librocksdb.a" ON)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)

find_package(RocksDB QUIET CONFIG)
if(NOT TARGET RocksDB::rocksdb)
  find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h)
  find_library(ROCKSDB_LIBRARY NAMES rocksdb librocksdb.a)
  if(ROCKSDB_INCLUDE_DIR AND ROCKSDB_LIBRARY)
    add_library(RocksDB::rocksdb UNKNOWN IMPORTED)
    set_target_properties(RocksDB::rocksdb PROPERTIES
      IMPORTED_LOCATION "${ROCKSDB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ROCKSDB_INCLUDE_DIR}"
    )
  else()
    message(FATAL_ERROR "RocksDB not found. Install RocksDB (static) or provide CMAKE_PREFIX_PATH/ROCKSDB_INCLUDE_DIR/ROCKSDB_LIBRARY.")
  endif()
endif()

add_library(hpserver_rocksdb INTERFACE)
target_link_libraries(hpserver_rocksdb INTERFACE RocksDB::rocksdb)

if(HPSERVER_LINK_ROCKSDB_STATIC_DEPS)
  set(ROCKSDB_STATIC_DEPS
    Threads::Threads
    ZLIB::ZLIB
    BZip2::BZip2
    ${CMAKE_DL_LIBS}
  )

  foreach(lib_name IN ITEMS snappy lz4 zstd uring)
    string(TOUPPER "${lib_name}" lib_var)
    find_library(${lib_var}_LIBRARY NAMES ${lib_name} REQUIRED)
    list(APPEND ROCKSDB_STATIC_DEPS ${${lib_var}_LIBRARY})
  endforeach()

  target_link_libraries(hpserver_rocksdb INTERFACE ${ROCKSDB_STATIC_DEPS})
endif()

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

add_library(hpserver_lib STATIC
  src/logger/logger.cpp
  src/net/core/poller.cpp
  src/net/core/socket_ops.cpp
  src/net/http/http_conn.cpp
  src/net/http/http_cache.cpp
  src/net/http/http_connection_io.cpp
  src/net/http/http_request_parser.cpp
  src/net/http/http_message_builder.cpp
  src/net/proxy/http_proxy.cpp
  src/net/core/ip_filter.cpp
  src/hpserver.cpp
)

target_link_libraries(hpserver_lib
  PUBLIC
    hpserver_headers
    hpserver_rocksdb
)

target_compile_definitions(hpserver_lib PUBLIC
  $<$<CONFIG:Release>:DISABLE_LOGGING>
)

add_executable(hpserver
  src/main.cpp
)

target_link_libraries(hpserver
  PRIVATE
    hpserver_lib
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
