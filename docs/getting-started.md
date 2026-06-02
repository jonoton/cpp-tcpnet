---
layout: default
---

[🏠 Home](./) | [Next: Basic Usage >](./basic-usage.html)
<hr>

# Getting Started

To use `cpp-tcpnet`, include the `cpptcpnet.hpp` header in your project. It also depends on `cpp-pubsub` and `cpp-asyncworker`.

## Integration

### 1. CMake: `FetchContent` (Recommended)

You can have CMake automatically download and integrate `cpp-tcpnet` and its dependencies. By default, it downloads and compiles LibreSSL from source to provide out-of-the-box SSL/TLS support.

The following CMake options can be configured before including `cpp-tcpnet`:
* **`CPPTCPNET_USE_LIBRESSL`** (default: `ON`): Builds LibreSSL from source. Set to `OFF` if you prefer to use your system's pre-installed OpenSSL instead (falls back to standard `find_package(OpenSSL REQUIRED)`).
* **`CPPTCPNET_LIBRESSL_VERSION`** (default: `"3.9.2"`): Specifies the version of LibreSSL to download and build.
* **`CPPTCPNET_ENABLE_SSL`** (default: `ON`): Enables SSL/TLS support entirely. Set to `OFF` to disable.

```cmake
include(FetchContent)

# Optional configuration overrides:
# set(CPPTCPNET_USE_LIBRESSL OFF CACHE BOOL "Use system OpenSSL")
# set(CPPTCPNET_LIBRESSL_VERSION "3.9.2" CACHE STRING "LibreSSL version")
# set(CPPTCPNET_ENABLE_SSL OFF CACHE BOOL "Enable SSL support")

FetchContent_Declare(
  cpptcpnet
  GIT_REPOSITORY https://github.com/jonoton/cpp-tcpnet.git
  GIT_TAG main
)
FetchContent_MakeAvailable(cpptcpnet)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE cpptcpnet::cpptcpnet)
```

### 2. Manual Compilation (Without CMake)

If you are compiling manually without CMake, you will need:
* The headers for `cpp-pubsub` and `cpp-asyncworker` in your include path.
* To define the `CPPTCPNET_SSL_SUPPORT` preprocessor macro if you want SSL.
* To link OpenSSL or LibreSSL libraries (`-lssl -lcrypto`).
* On Linux, you must link the `pthread` library. On Windows, `ws2_32.lib` is automatically linked via `#pragma comment`.

```bash
# Compiling with SSL/TLS support:
g++ -std=c++17 -pthread -DCPPTCPNET_SSL_SUPPORT -I/path/to/cpp-pubsub -I/path/to/cpp-asyncworker main.cpp -lssl -lcrypto -o my_app

# Compiling without SSL/TLS support:
g++ -std=c++17 -pthread -I/path/to/cpp-pubsub -I/path/to/cpp-asyncworker main.cpp -o my_app
```

---
[🏠 Home](./) | [Next: Basic Usage >](./basic-usage.html)
