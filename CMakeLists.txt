cmake_minimum_required(VERSION 3.10)
project(cpptcpnet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CPPTCPNET_PUBSUB_TAG "main" CACHE STRING "Git tag/branch to use for cpp-pubsub dependency")
FetchContent_Declare(
  cpppubsub
  GIT_REPOSITORY https://github.com/jonoton/cpp-pubsub.git
  GIT_TAG ${CPPTCPNET_PUBSUB_TAG}
)
FetchContent_MakeAvailable(cpppubsub)

set(CPPTCPNET_ASYNCWORKER_TAG "main" CACHE STRING "Git tag/branch to use for cpp-asyncworker dependency")
FetchContent_Declare(
  cppasyncworker
  GIT_REPOSITORY https://github.com/jonoton/cpp-asyncworker.git
  GIT_TAG ${CPPTCPNET_ASYNCWORKER_TAG}
)
FetchContent_MakeAvailable(cppasyncworker)

# Option to enable SSL support
option(CPPTCPNET_ENABLE_SSL "Enable SSL/TLS support using OpenSSL" ON)

if(CPPTCPNET_ENABLE_SSL)
    option(CPPTCPNET_USE_LIBRESSL "Build LibreSSL from source instead of finding system OpenSSL" ON)

    if(CPPTCPNET_USE_LIBRESSL)
        include(FetchContent)
        # Use CMAKE_POLICY_DEFAULT_CMPxxx variables to ensure policy choices propagate
        # to LibreSSL's subproject even after it calls cmake_minimum_required()
        set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
        set(CMAKE_POLICY_DEFAULT_CMP194 NEW)

        set(CPPTCPNET_LIBRESSL_VERSION "3.9.2" CACHE STRING "Version of LibreSSL to use")

        # Disable LibreSSL apps and tests to keep build times fast
        set(LIBRESSL_APPS OFF CACHE BOOL "Build LibreSSL apps" FORCE)
        set(LIBRESSL_TESTS OFF CACHE BOOL "Build LibreSSL tests" FORCE)
        set(USE_STATIC_MSVC_RUNTIMES OFF CACHE BOOL "Use static MSVC runtimes" FORCE)

        if(WIN32)
            # Disable assembly on Windows to prevent register corruption/ABI mismatch crashes
            # in LibreSSL assembly optimizations (e.g. stack alignment or register preservation issues)
            set(ENABLE_ASM OFF CACHE BOOL "Enable assembly" FORCE)
        endif()

        FetchContent_Declare(
            libressl
            URL "https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${CPPTCPNET_LIBRESSL_VERSION}.tar.gz"
        )
        FetchContent_MakeAvailable(libressl)

        # Patch LibreSSL to prevent Debug CRT assertion crash in is_socket on Windows
        set(PATCH_FILE "${libressl_SOURCE_DIR}/crypto/compat/posix_win.c")
        if(EXISTS "${PATCH_FILE}")
            file(READ "${PATCH_FILE}" FILE_CONTENT)
            if(NOT FILE_CONTENT MATCHES "getsockopt\\(\\(SOCKET\\)fd")
                string(REGEX REPLACE "static int[ \t\r\n]+is_socket\\(int fd\\)[ \t\r\n]*\\{[^}]*if[ \t\r\n]*\\([^{}]*\\)[ \t\r\n]*\\{[^{}]*\\}[^{}]*\\}" "static int\nis_socket(int fd)\n{\n\tint optval;\n\tint optlen = sizeof(optval);\n\tif (getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE, (char *)&optval, &optlen) == 0) {\n\t\treturn 1; /* fd is a socket */\n\t}\n\treturn 0; /* fd is not a socket */\n}" MODIFIED_CONTENT "${FILE_CONTENT}")
                file(WRITE "${PATCH_FILE}" "${MODIFIED_CONTENT}")
                message(STATUS "[cpptcpnet] Patched LibreSSL posix_win.c to prevent Debug CRT crash")
            endif()
        endif()

        # Alias targets to match the OpenSSL target names expected by cpp-tcpnet
        if(NOT TARGET OpenSSL::Crypto)
            add_library(OpenSSL::Crypto ALIAS crypto)
        endif()
        if(NOT TARGET OpenSSL::SSL)
            add_library(OpenSSL::SSL ALIAS ssl)
        endif()
    else()
        find_package(OpenSSL REQUIRED)
    endif()
endif()

# Define as an interface library (header-only)
add_library(cpptcpnet INTERFACE)
add_library(cpptcpnet::cpptcpnet ALIAS cpptcpnet)

target_include_directories(cpptcpnet INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(cpptcpnet INTERFACE cxx_std_17)
if(WIN32)
    target_compile_definitions(cpptcpnet INTERFACE NOMINMAX)
endif()

find_package(Threads REQUIRED)
target_link_libraries(cpptcpnet INTERFACE Threads::Threads cpppubsub::cpppubsub cppasyncworker::cppasyncworker)
if(CPPTCPNET_ENABLE_SSL)
    target_link_libraries(cpptcpnet INTERFACE OpenSSL::SSL)
    target_compile_definitions(cpptcpnet INTERFACE CPPTCPNET_SSL_SUPPORT)
endif()

# Determine if this is the top-level project
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(IS_TOP_LEVEL ON)
else()
    set(IS_TOP_LEVEL OFF)
endif()

# Option to build examples
option(CPPTCPNET_BUILD_EXAMPLES "Build the example programs" ${IS_TOP_LEVEL})

if(CPPTCPNET_BUILD_EXAMPLES)
    string(REPLACE "\\" "/" SAFE_PROJECT_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
    file(GLOB EXAMPLE_SOURCES "examples/*.cpp")
    foreach(EXAMPLE_SOURCE ${EXAMPLE_SOURCES})
        get_filename_component(EXAMPLE_NAME ${EXAMPLE_SOURCE} NAME_WE)
        add_executable(${EXAMPLE_NAME} ${EXAMPLE_SOURCE})
        target_link_libraries(${EXAMPLE_NAME} PRIVATE cpptcpnet::cpptcpnet)
        target_compile_definitions(${EXAMPLE_NAME} PRIVATE CPPTCPNET_CERTS_DIR="${SAFE_PROJECT_SOURCE_DIR}/certs")
    endforeach()
endif()

# Option to build tests
option(CPPTCPNET_BUILD_TESTS "Build the unit tests" ${IS_TOP_LEVEL})

if(CPPTCPNET_BUILD_TESTS)
    enable_testing()
    
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    
    add_subdirectory(tests)
endif()
