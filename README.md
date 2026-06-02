# cpp-tcpnet

`cpp-tcpnet` is a fast, cross-platform, and asynchronous C++17 library for TCP networking. It provides robust server (`TcpListener`) and client (`TcpClient`) abstractions powered by a non-blocking event loop and background thread pools.

## Features
- **Header-Only:** Include `cpptcpnet.hpp` in your project.
- **Cross-Platform:** Native support for Windows (`Winsock2`) and POSIX (`poll`).
- **DNS Resolution & IPv6:** Resolve hostnames dynamically on client connections. Fully supports both IPv4 and IPv6 (`AF_INET6`), including dual-stack wildcard binding.
- **SSL/TLS Support:** Secure connections with OpenSSL (TLS 1.2+).
- **Asynchronous & Non-Blocking:** High-performance background event loop ensures the main thread is never stalled by network IO.
- **Built-in Thread Pool:** Automatically dispatches incoming packet data to a background worker pool for concurrent processing.
- **Event-Driven:** Uses the `cpp-pubsub` broker to publish events whenever connections connect or disconnect.
- **Peer Address API:** Retrieve the remote peer's IP address and port for any active session with `GetPeerAddress(session_id)`.
- **Highly Configurable:** Fine-tune socket options (Nagle's algorithm, keepalives, linger, reuse port), thread pool bounds, outbound buffer limits, dynamically allocated receive buffers, custom cipher suites, TLS version bounds, idle timeouts, and automatic client reconnection with backoff.
- **Performance Metrics:** Monitor cumulative bytes/packets sent and received, connection counts, calculate real-time throughput using the sliding-window `ThroughputTracker`, and scale raw counts using formatting helpers.
- **Robust Error Handling:** Synchronous setup methods use exceptions (`std::system_error`), while asynchronous background errors are reported via callbacks and PubSub events.

## Integration

By default, SSL/TLS support is enabled and fetches and builds LibreSSL from source. 

The following CMake options are available to customize this behavior:
* **`CPPTCPNET_USE_LIBRESSL`** (default: `ON`): Builds LibreSSL from source. Set to `OFF` if you prefer to use your system's pre-installed OpenSSL instead (which falls back to `find_package(OpenSSL REQUIRED)`).
* **`CPPTCPNET_LIBRESSL_VERSION`** (default: `"3.9.2"`): Specifies the version of LibreSSL to download and build.
* **`CPPTCPNET_ENABLE_SSL`** (default: `ON`): Enables SSL/TLS support entirely. Set to `OFF` to disable.

```cmake
include(FetchContent)

# Optional configuration overrides:
# set(CPPTCPNET_USE_LIBRESSL OFF CACHE BOOL "Use system OpenSSL")
# set(CPPTCPNET_LIBRESSL_VERSION "3.9.2" CACHE STRING "LibreSSL version")

FetchContent_Declare(
  cpptcpnet
  GIT_REPOSITORY https://github.com/jonoton/cpp-tcpnet.git
  GIT_TAG main
)
FetchContent_MakeAvailable(cpptcpnet)

target_link_libraries(your_target PRIVATE cpptcpnet::cpptcpnet)
```

## Quick Start (Server)

```cpp
#include "cpptcpnet.hpp"
#include <iostream>

int main() {
    cpptcpnet::TcpListener server(8080);
    
    server.SetDataHandler([](uint64_t session_id, const std::vector<uint8_t>& data) {
        std::string text(data.begin(), data.end());
        std::cout << "Received: " << text << std::endl;
    });

    try {
        server.Start();
        std::cout << "Server running. Press Enter to stop." << std::endl;
        std::cin.get();
        server.Stop();
    } catch (const std::exception& e) {
        std::cerr << "Failed to start server: " << e.what() << std::endl;
    }

    return 0;
}
```

## Performance Metrics & Throughput

You can query server or client stats at any time, or track real-time throughput rates:

```cpp
// 1. Query cumulative stats synchronously
cpptcpnet::ListenerStats stats = server.GetStats();
std::cout << "Bytes Received: " << stats.bytes_received << " (" 
          << stats.active_connections << " active connections)\n";

// 2. Track real-time throughput rate (sliding-window) asynchronously
cpptcpnet::ThroughputTracker tracker(server.GetEventBroker(), std::chrono::seconds(1));
std::cout << "Receive throughput: " << tracker.GetRecvThroughputBytesPerSec() << " B/s\n";
```

## Configuration & Tuning Options

`cpp-tcpnet` is designed to be highly configurable. You can configure timeouts, socket tuning, buffer sizes, and connection recovery parameters on both `TcpListener` and `TcpClient`:

```cpp
cpptcpnet::TcpClient client;

// Disable Nagle's algorithm for low-latency interactive protocols
client.SetNoDelay(true);

// Set application-level read/write sizes & worker threads
client.SetRecvBufferSize(16384);  // 16KB read buffer
client.SetWorkerThreadCount(4);    // 4 background worker threads

// Set maximum outbound buffer limit (default: 10MB) to prevent unbounded memory growth
client.SetMaxOutboundBufferSize(50 * 1024 * 1024); // 50MB

// Direct error handler callback for asynchronous background errors
client.SetErrorHandler([](int error_code, const std::string& message) {
    std::cerr << "Client background error [" << error_code << "]: " << message << std::endl;
});

// Enable automatic reconnection on connection drops with exponential backoff
client.SetAutoReconnect(true, std::chrono::seconds(1), std::chrono::seconds(30));

// Configure socket keepalive parameters
cpptcpnet::KeepAliveConfig ka;
ka.enabled = true;
ka.idle_secs = 30;
ka.interval_secs = 10;
ka.count = 3;
client.SetKeepAliveConfig(ka);
```

## Documentation
For full API documentation and advanced usage, please refer to our [GitHub Pages Documentation Site](https://jonoton.github.io/cpp-tcpnet/).
