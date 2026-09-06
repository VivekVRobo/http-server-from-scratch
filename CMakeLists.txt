cmake_minimum_required(VERSION 3.20)
project(vhttp VERSION 0.1.0 LANGUAGES CXX)

option(VHTTP_BUILD_TESTS "Build vhttp tests" ON)
option(VHTTP_BUILD_EXAMPLES "Build vhttp examples" ON)

add_library(vhttp
    src/net/socket.cpp
    src/http/parser.cpp
    src/http/response.cpp
    src/router/router.cpp
    src/server/connection.cpp
    src/server/connection_session.cpp
    src/server/server.cpp
    src/server/thread_pool_runtime.cpp
    src/static_files/static_file_handler.cpp
)

add_library(vhttp::vhttp ALIAS vhttp)

find_package(Threads REQUIRED)

target_compile_features(vhttp PUBLIC cxx_std_20)
target_include_directories(vhttp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(vhttp PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(vhttp PRIVATE /W4 /permissive-)
    target_link_libraries(vhttp PUBLIC ws2_32)
else()
    target_compile_options(vhttp PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(VHTTP_BUILD_EXAMPLES)
    add_executable(vhttp_hello examples/hello_world/main.cpp)
    target_link_libraries(vhttp_hello PRIVATE vhttp::vhttp)

    add_executable(vhttp_bench_server examples/benchmark_server/main.cpp)
    target_link_libraries(vhttp_bench_server PRIVATE vhttp::vhttp)
endif()

include(CTest)
if(BUILD_TESTING AND VHTTP_BUILD_TESTS)
    add_executable(vhttp_parser_tests tests/parser_tests.cpp)
    target_link_libraries(vhttp_parser_tests PRIVATE vhttp::vhttp)
    add_test(NAME parser_tests COMMAND vhttp_parser_tests)

    add_executable(vhttp_framing_tests tests/framing_tests.cpp)
    target_link_libraries(vhttp_framing_tests PRIVATE vhttp::vhttp)
    add_test(NAME framing_tests COMMAND vhttp_framing_tests)

    add_executable(vhttp_response_tests tests/response_tests.cpp)
    target_link_libraries(vhttp_response_tests PRIVATE vhttp::vhttp)
    add_test(NAME response_tests COMMAND vhttp_response_tests)

    add_executable(vhttp_router_tests tests/router_tests.cpp)
    target_link_libraries(vhttp_router_tests PRIVATE vhttp::vhttp)
    add_test(NAME router_tests COMMAND vhttp_router_tests)

    add_executable(vhttp_connection_policy_tests tests/connection_policy_tests.cpp)
    target_link_libraries(vhttp_connection_policy_tests PRIVATE vhttp::vhttp)
    add_test(NAME connection_policy_tests COMMAND vhttp_connection_policy_tests)

    add_executable(vhttp_connection_integration_tests tests/connection_integration_tests.cpp)
    target_link_libraries(vhttp_connection_integration_tests PRIVATE vhttp::vhttp)
    add_test(NAME connection_integration_tests COMMAND vhttp_connection_integration_tests)

    add_executable(vhttp_connection_session_tests tests/connection_session_tests.cpp)
    target_link_libraries(vhttp_connection_session_tests PRIVATE vhttp::vhttp)
    add_test(NAME connection_session_tests COMMAND vhttp_connection_session_tests)

    add_executable(vhttp_static_file_tests tests/static_file_tests.cpp)
    target_link_libraries(vhttp_static_file_tests PRIVATE vhttp::vhttp)
    add_test(NAME static_file_tests COMMAND vhttp_static_file_tests)

    add_executable(vhttp_static_file_integration_tests tests/static_file_integration_tests.cpp)
    target_link_libraries(vhttp_static_file_integration_tests PRIVATE vhttp::vhttp)
    add_test(NAME static_file_integration_tests COMMAND vhttp_static_file_integration_tests)

    add_executable(vhttp_thread_pool_runtime_tests tests/thread_pool_runtime_tests.cpp)
    target_link_libraries(vhttp_thread_pool_runtime_tests PRIVATE vhttp::vhttp)
    add_test(NAME thread_pool_runtime_tests COMMAND vhttp_thread_pool_runtime_tests)
endif()
