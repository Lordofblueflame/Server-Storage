cmake_minimum_required(VERSION 3.20)

project(localedge_server VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(HOST_INDEXER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../HostIndexer")

set(HOST_INDEXER_CODEC_SOURCES
    "${HOST_INDEXER_ROOT}/domain/validation/validators.cpp"
    "${HOST_INDEXER_ROOT}/domain/validation/validate_snapshot.cpp"
    "${HOST_INDEXER_ROOT}/domain/serialization/serialization.cpp"
)

set(LOCALEDGE_GATEWAY_SOURCES
    src/net/hostindexer_client.cpp
    src/api/frontend_api_server.cpp
    src/api/frontend_dto.cpp
    src/api/jwt_auth.cpp
    src/api/command_load_balancer.cpp
    src/ingest/ingest_coordinator.cpp
    src/ingest/message_validator.cpp
    src/protocol/hostindexer_frame.cpp
    src/protocol/wire.cpp
    src/storage/in_memory_ingest_store.cpp
    src/observability/metrics.cpp
    src/sinks/materializer_sink.cpp
    ${HOST_INDEXER_CODEC_SOURCES}
)

add_library(localedge_gateway STATIC ${LOCALEDGE_GATEWAY_SOURCES})
target_include_directories(localedge_gateway PUBLIC
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${HOST_INDEXER_ROOT}"
    "${HOST_INDEXER_ROOT}/domain"
    "${HOST_INDEXER_ROOT}/lib/nlohmann"
)
target_link_libraries(localedge_gateway
    PUBLIC
    Boost::system
    Threads::Threads
    OpenSSL::Crypto
)
if (TARGET Boost::throw_exception)
    target_link_libraries(localedge_gateway PUBLIC Boost::throw_exception)
endif()

add_executable(localedge_server main.cpp)
target_link_libraries(localedge_server PRIVATE localedge_gateway)

add_executable(localedge_server_tests
    tests/test_main.cpp
    tests/protocol_wire_tests.cpp
    tests/hostindexer_frame_tests.cpp
    tests/ingest_coordinator_tests.cpp
    tests/hostindexer_client_integration_tests.cpp
)
target_link_libraries(localedge_server_tests PRIVATE localedge_gateway)

enable_testing()
add_test(NAME localedge_server_tests COMMAND localedge_server_tests)

if (MSVC)
    target_compile_options(localedge_gateway PRIVATE /W4)
    target_compile_options(localedge_server PRIVATE /W4)
    target_compile_options(localedge_server_tests PRIVATE /W4)
else()
    target_compile_options(localedge_gateway PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(localedge_server PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(localedge_server_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
