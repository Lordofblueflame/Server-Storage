cmake_minimum_required(VERSION 3.20)

project(HostIndexer VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_SOURCE_DIR}/lib/nlohmann
)

add_library(lmdb STATIC
    ${CMAKE_SOURCE_DIR}/lib/liblmdb/mdb.c
    ${CMAKE_SOURCE_DIR}/lib/liblmdb/midl.c
)
target_include_directories(lmdb PUBLIC
    ${CMAKE_SOURCE_DIR}/lib/liblmdb
)

set(HOST_INDEXER_CODEC_SOURCES
    domain/validation/validators.cpp
    domain/validation/validate_snapshot.cpp
    domain/serialization/serialization.cpp
    domain/migration/migration.cpp
)

add_library(host_indexer_codec STATIC ${HOST_INDEXER_CODEC_SOURCES})
target_include_directories(host_indexer_codec PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/domain
    ${CMAKE_SOURCE_DIR}/utils
)
target_link_libraries(host_indexer_codec PUBLIC nlohmann_json)

set(HOST_INDEXER_ENGINE_SOURCES
    filesystem/filesystem_scanner.cpp
    snapshot/snapshot_builder.cpp
    snapshot/delta_engine.cpp
    storage/lmdb_store.cpp
    api/indexing_pipeline.cpp
    transport/ws_crud_server.cpp
)

add_library(host_indexer_lib STATIC ${HOST_INDEXER_ENGINE_SOURCES})
target_include_directories(host_indexer_lib PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/domain
    ${CMAKE_SOURCE_DIR}/filesystem
    ${CMAKE_SOURCE_DIR}/snapshot
    ${CMAKE_SOURCE_DIR}/storage
    ${CMAKE_SOURCE_DIR}/api
    ${CMAKE_SOURCE_DIR}/transport
    ${CMAKE_SOURCE_DIR}/utils
)
target_link_libraries(host_indexer_lib PUBLIC
    host_indexer_codec
    lmdb
    Boost::system
    Threads::Threads
)

add_executable(host_indexer_cli main.cpp)
target_link_libraries(host_indexer_cli PRIVATE host_indexer_lib)

add_executable(host_indexer_tests tests/indexer_tests.cpp)
target_link_libraries(host_indexer_tests PRIVATE host_indexer_lib)

enable_testing()
add_test(NAME host_indexer_tests COMMAND host_indexer_tests)

if (MSVC)
    target_compile_options(lmdb PRIVATE /W3)
    target_compile_options(host_indexer_codec PRIVATE /W4)
    target_compile_options(host_indexer_lib PRIVATE /W4)
    target_compile_options(host_indexer_cli PRIVATE /W4)
    target_compile_options(host_indexer_tests PRIVATE /W4)
else()
    target_compile_options(lmdb PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(host_indexer_codec PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(host_indexer_lib PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(host_indexer_cli PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(host_indexer_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
