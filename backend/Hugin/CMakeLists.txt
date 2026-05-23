cmake_minimum_required(VERSION 3.20)

project(hugin VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(MUNINN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Muninn")

set(MUNINN_CODEC_SOURCES
    "${MUNINN_ROOT}/domain/validation/validators.cpp"
    "${MUNINN_ROOT}/domain/validation/validate_snapshot.cpp"
    "${MUNINN_ROOT}/domain/serialization/serialization.cpp"
)

set(HUGIN_GATEWAY_SOURCES
    net/muninn_client.cpp
    api/frontend_api_server.cpp
    api/frontend_dto.cpp
    api/jwt_auth.cpp
    api/command_load_balancer.cpp
    ingest/ingest_coordinator.cpp
    ingest/message_validator.cpp
    protocol/muninn_frame.cpp
    protocol/wire.cpp
    storage/in_memory_ingest_store.cpp
    observability/metrics.cpp
    sinks/materializer_sink.cpp
    ${MUNINN_CODEC_SOURCES}
)

add_library(hugin_gateway STATIC ${HUGIN_GATEWAY_SOURCES})
target_include_directories(hugin_gateway PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${MUNINN_ROOT}"
    "${MUNINN_ROOT}/domain"
)
target_link_libraries(hugin_gateway
    PUBLIC
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    Boost::system
    Threads::Threads
    OpenSSL::Crypto
)
if (TARGET Boost::throw_exception)
    target_link_libraries(hugin_gateway PUBLIC Boost::throw_exception)
endif()

add_executable(hugin main.cpp)
target_link_libraries(hugin PRIVATE hugin_gateway)

add_executable(hugin_tests
    tests/test_main.cpp
    tests/protocol_wire_tests.cpp
    tests/muninn_frame_tests.cpp
    tests/ingest_coordinator_tests.cpp
    tests/muninn_client_integration_tests.cpp
    tests/jwt_auth_tests.cpp
    tests/frontend_dto_tests.cpp
)
target_link_libraries(hugin_tests PRIVATE hugin_gateway)

enable_testing()
add_test(NAME hugin_tests COMMAND hugin_tests)

if (MSVC)
    target_compile_options(hugin_gateway PRIVATE /W4)
    target_compile_options(hugin PRIVATE /W4)
    target_compile_options(hugin_tests PRIVATE /W4)
else()
    target_compile_options(hugin_gateway PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(hugin PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(hugin_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
