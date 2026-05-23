cmake_minimum_required(VERSION 3.20)

project(Muninn VERSION 0.3.0 LANGUAGES C CXX)

# CMake 4 warns about FindBoost policy unless explicitly set.
if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
option(MUNINN_ENABLE_LEGACY_TRANSPORT_TESTS "Build legacy transport-centric tests." OFF)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_path(LMDB_INCLUDE_DIR NAMES lmdb.h REQUIRED)
find_library(LMDB_LIBRARY NAMES lmdb liblmdb REQUIRED)
if (BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
endif()

set(MUNINN_CODEC_SOURCES
    domain/validation/validators.cpp
    domain/validation/validate_snapshot.cpp
    domain/serialization/serialization.cpp
    domain/migration/migration.cpp
)

add_library(muninn_codec STATIC ${MUNINN_CODEC_SOURCES})
target_include_directories(muninn_codec PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/domain
    ${CMAKE_SOURCE_DIR}/utils
)
target_link_libraries(muninn_codec PUBLIC nlohmann_json::nlohmann_json)

set(MUNINN_CORE_SOURCES
    filesystem/filesystem_scanner.cpp
    snapshot/snapshot_builder.cpp
    snapshot/delta_engine.cpp
    storage/lmdb_store.cpp
    api/indexing_pipeline.cpp
)

set(MUNINN_TRANSPORT_SOURCES
    transport/crud_command_processor.cpp
    transport/ws_crud_server.cpp
)

add_library(muninn_core_lib STATIC ${MUNINN_CORE_SOURCES})
target_include_directories(muninn_core_lib PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/domain
    ${CMAKE_SOURCE_DIR}/filesystem
    ${CMAKE_SOURCE_DIR}/snapshot
    ${CMAKE_SOURCE_DIR}/storage
    ${CMAKE_SOURCE_DIR}/api
    ${CMAKE_SOURCE_DIR}/transport
    ${CMAKE_SOURCE_DIR}/utils
)
target_link_libraries(muninn_core_lib PUBLIC
    muninn_codec
    "${LMDB_LIBRARY}"
    Boost::system
    Threads::Threads
)
target_include_directories(muninn_core_lib PUBLIC
    "${LMDB_INCLUDE_DIR}"
)
if (TARGET Boost::throw_exception)
    target_link_libraries(muninn_core_lib PUBLIC Boost::throw_exception)
endif()

add_library(muninn_lib STATIC ${MUNINN_TRANSPORT_SOURCES})
target_include_directories(muninn_lib PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/transport
)
target_link_libraries(muninn_lib PUBLIC muninn_core_lib)

add_executable(muninn_cli main.cpp)
target_link_libraries(muninn_cli PRIVATE muninn_lib)

if (BUILD_TESTING)
    add_executable(muninn_core_tests tests/core_tests.cpp)
    target_link_libraries(muninn_core_tests PRIVATE muninn_core_lib GTest::gtest_main)
    add_test(NAME muninn_core_tests COMMAND muninn_core_tests)

    if (MUNINN_ENABLE_LEGACY_TRANSPORT_TESTS)
        add_executable(muninn_tests tests/indexer_tests.cpp)
        target_link_libraries(muninn_tests PRIVATE muninn_lib)
        add_test(NAME muninn_tests COMMAND muninn_tests)
    endif()
endif()

if (MSVC)
    target_compile_options(muninn_codec PRIVATE /W4 /EHsc)
    target_compile_options(muninn_core_lib PRIVATE /W4 /EHsc)
    target_compile_options(muninn_lib PRIVATE /W4 /EHsc)
    target_compile_options(muninn_cli PRIVATE /W4 /EHsc)
    if (BUILD_TESTING)
        target_compile_options(muninn_core_tests PRIVATE /W4 /EHsc)
    endif()
    if (BUILD_TESTING AND MUNINN_ENABLE_LEGACY_TRANSPORT_TESTS)
        target_compile_options(muninn_tests PRIVATE /W4 /EHsc)
    endif()
else()
    target_compile_options(muninn_codec PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(muninn_core_lib PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(muninn_lib PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(muninn_cli PRIVATE -Wall -Wextra -Wpedantic)
    if (BUILD_TESTING)
        target_compile_options(muninn_core_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if (BUILD_TESTING AND MUNINN_ENABLE_LEGACY_TRANSPORT_TESTS)
        target_compile_options(muninn_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
