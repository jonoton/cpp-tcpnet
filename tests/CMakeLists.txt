# Replace backslashes with forward slashes to prevent C++ escape character issues on Windows
string(REPLACE "\\" "/" SAFE_PROJECT_SOURCE_DIR "${PROJECT_SOURCE_DIR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/test_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/test_config.hpp"
)

add_executable(test_tcpnet test_tcpnet.cpp)
target_link_libraries(test_tcpnet PRIVATE cpptcpnet::cpptcpnet GTest::gtest_main)
target_include_directories(test_tcpnet PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME TcpNetTests COMMAND test_tcpnet)
