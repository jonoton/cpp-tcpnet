---
layout: default
---

[< Previous: Getting Started](./getting-started.html) | [🏠 Home](./) | [Next: Advanced Usage >](./advanced-usage.html)
<hr>

# Basic Usage (Server)

The `TcpListener` class allows you to create a high-performance TCP server that automatically dispatches incoming data to a background worker pool.

## Starting the Server

```cpp
#include "cpptcpnet.hpp"
#include <iostream>

int main() {
    // Construct TcpListener. Can bind to IPv4 (e.g., "0.0.0.0"), IPv6 (e.g., "::", "::1"), or hostname (e.g., "localhost").
    // Binding to "::" enables dual-stack support by default on supported operating systems.
    cpptcpnet::TcpListener server(8080, "0.0.0.0");
    
    // Set a handler to process incoming data
    server.SetDataHandler([](uint64_t session_id, const std::vector<uint8_t>& data) {
        std::string text(data.begin(), data.end());
        std::cout << "Received data from " << session_id << ": " << text << std::endl;
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

## Error Handling

`cpp-tcpnet` splits error handling into two categories: synchronous and asynchronous.

1. **Synchronous Errors**: Methods like `Start()` and `Connect()` are called directly by your application. If they fail (e.g., port in use, invalid IP), they will throw a `std::system_error` or `std::runtime_error`.
2. **Asynchronous Errors**: If a failure occurs in the background polling thread, it is broadcasted over the internal PubSub broker to the `"error_events"` topic as a `cpptcpnet::ErrorEvent`. Alternatively, you can use `server.SetErrorHandler` for a direct callback.

Here is an example of subscribing to background error events via the PubSub broker:
```cpp
auto err_sub = server.GetEventBroker().Subscribe<cpptcpnet::ErrorEvent>("error_events");
worker.AddSubscription<cpptcpnet::ErrorEvent>(err_sub, [](const cpptcpnet::ErrorEvent& event) {
    std::cerr << "Background error [" << event.error_code << "]: " << event.message << std::endl;
});
```

And here is an example of setting a direct error handler callback on the server:
```cpp
server.SetErrorHandler([](int error_code, const std::string& message) {
    std::cerr << "Direct callback error [" << error_code << "]: " << message << std::endl;
});
```

## Connection State Events

You can use the built-in `cpp-pubsub` broker to listen for connection lifecycle events (connect/disconnect).

```cpp
cpppubsub::Worker worker;
auto sub = server.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");

worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [&server](const cpptcpnet::ConnectionEvent& event) {
    if (event.state == cpptcpnet::ConnectionState::Connected) {
        std::cout << "Client connected: " << event.session_id << std::endl;
        server.Send(event.session_id, "Welcome to the server!\n");
    } else {
        std::cout << "Client disconnected: " << event.session_id << std::endl;
    }
});

// Start the worker thread
worker.Start();
```

## Logging

You can configure a global logger callback to capture internal logs from the `cpp-tcpnet` library. This allows you to integrate the library's logging with your own application's log system.

```cpp
cpptcpnet::SetLogger([](cpptcpnet::LogSeverity severity, const std::string& className, const std::string& message) {
    std::string sevStr;
    switch (severity) {
        case cpptcpnet::LogSeverity::Debug: sevStr = "DEBUG"; break;
        case cpptcpnet::LogSeverity::Info:  sevStr = "INFO"; break;
        case cpptcpnet::LogSeverity::Warn:  sevStr = "WARN"; break;
        case cpptcpnet::LogSeverity::Error: sevStr = "ERROR"; break;
    }
    std::cout << "[" << sevStr << "] " << className << ": " << message << std::endl;
});
```

## Server Configuration & Resource Management

`TcpListener` provides a comprehensive set of configuration methods to control socket-level behavior, resource bounds, thread pooling, and TLS parameters:

### Timeouts & Throttling
* **Idle Timeout**: Drops connections that have been inactive (no read/write activity) for longer than the specified duration.
* **Send Timeout**: Drops connections if pending outbound data remains unflushed for longer than the timeout window.
* **Throttle Cooldown**: The duration the server temporarily stops accepting new connections when running out of file descriptors.
* **SSL Handshake Timeout**: The maximum duration permitted to complete the SSL/TLS handshake before the connection is closed.

```cpp
server.SetIdleTimeout(std::chrono::seconds(60));            // Default: 60s
server.SetSendTimeout(std::chrono::seconds(30));            // Default: 30s
server.SetThrottleCooldown(std::chrono::milliseconds(200)); // Default: 200ms
server.SetSslHandshakeTimeout(std::chrono::seconds(10));    // Default: 10s
```

### Socket-Level Tuning
* **TCP_NODELAY**: Controls Nagle's algorithm (disables latency-focused coalescing for low-latency interactive protocols).
* **SO_RCVBUF / SO_SNDBUF**: Configures the kernel socket receive/send buffer sizes.
* **SO_KEEPALIVE Tuning**: Enables and configures TCP keepalive probes (idle time, interval, and max counts).
* **SO_LINGER**: Configures socket linger behavior on close to block or perform RST teardown.
* **SO_REUSEPORT**: Enables multiple processes or threads to bind to the same port for kernel-level load balancing (POSIX platforms only).

```cpp
server.SetNoDelay(true); // Disable Nagle's algorithm

// Set kernel socket buffers
server.SetSocketRecvBufferSize(65536);
server.SetSocketSendBufferSize(65536);

// Tune Keep-Alives
cpptcpnet::KeepAliveConfig ka_config;
ka_config.enabled = true;
ka_config.idle_secs = 30;     // 30s idle before first probe
ka_config.interval_secs = 10; // 10s between probes
ka_config.count = 3;          // 3 failed probes = disconnect
server.SetKeepAliveConfig(ka_config);

// Set Linger on Close
server.SetLinger(true, 5); // Linger for 5 seconds

// Reuse Port (POSIX only)
server.SetReusePort(true);
```

### Backlog & Threading & Buffers
* **Listen Backlog**: The maximum size of the queue of pending connections.
* **Worker Thread Count**: The size of the thread pool processing data and error callbacks. If set to 0, automatically defaults to `std::thread::hardware_concurrency()`. Under the hood, this creates a vector of single-threaded worker pools. Incoming data callbacks for any single session are pinned to the same worker pool using a hash of the session ID. This guarantees **session affinity** (serial, in-order execution of callbacks for any single session) while allowing concurrent processing across different sessions.
* **Receive Buffer Size**: The application-level receive buffer size. A hybrid stack/heap allocator is used to dynamically size the buffer on data reads.
* **Send Chunk Size**: The maximum segment size transmitted per socket send operation.
* **Max Outbound Buffer Size**: The maximum accumulated outbound buffer size (in bytes) allowed per connection. Defaults to 10MB (`10 * 1024 * 1024`). If a client's queued send data exceeds this, further writes fail to prevent unbounded memory growth.

```cpp
server.SetListenBacklog(SOMAXCONN);
server.SetWorkerThreadCount(4);  // Use 4 worker threads
server.SetRecvBufferSize(16384); // 16KB read buffer
server.SetSendChunkSize(32768);  // 32KB send chunks
server.SetMaxOutboundBufferSize(50 * 1024 * 1024); // 50MB limit
```

### Querying Configuration at Runtime
All configuration settings have corresponding getter methods to query the current values at runtime. For example:
```cpp
bool no_delay = server.GetNoDelay();
size_t max_buf = server.GetMaxOutboundBufferSize();
std::chrono::milliseconds idle = server.GetIdleTimeout();
```

### Session Inspection & Management
You can programmatically retrieve a peer's IP address and port, disconnect individual clients by session ID, check if the server is running, or query the list of active connections:
```cpp
if (server.IsRunning()) {
    std::vector<uint64_t> sessions = server.GetActiveSessions();
    for (uint64_t session_id : sessions) {
        // Retrieve the remote client's peer address (IP and port)
        try {
            cpptcpnet::PeerAddress peer = server.GetPeerAddress(session_id);
            std::cout << "Session: " << session_id << " connected from " << peer.ip << ":" << peer.port << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Failed to query peer address: " << e.what() << std::endl;
        }

        // Force disconnect a client
        server.Disconnect(session_id);
    }
}
```

## Connection Profiles

To simplify network tuning across various network conditions (such as high latency, low bandwidth, or high-speed local networks), `cpp-tcpnet` provides a profile-based configuration model. Instead of setting multiple individual configuration options, you can pack these options into a `ConnectionProfile` struct and apply it either globally as a default, or dynamically to specific active connections on the fly.

### Profile Presets

`ConnectionProfile` provides three standard static factory functions for common scenarios:

1. **`ConnectionProfile::HighLatency()`**: Optimized for networks with high latency and high bandwidth-delay product (BDP), such as satellite links.
   - Enforces `no_delay = true`
   - Configures socket buffer sizes to `512 KB`
   - Configures application-level receive buffer size to `64 KB`
   - Increases connection timeouts (e.g. `120s` idle timeout)

2. **`ConnectionProfile::LowBandwidth()`**: Optimized for low bandwidth, high latency, or metered networks like cellular 3G/4G.
   - Disables `no_delay` (enables Nagle's algorithm) to merge small packets and reduce protocol overhead
   - Configures socket buffer sizes to `16 KB`
   - Configures application-level receive buffer size to `1 KB` to prevent bufferbloat
   - Implements longer idle and keepalive intervals

3. **`ConnectionProfile::ReliableLAN()`**: Optimized for low-latency, high-speed, reliable local area networks.
   - Enforces `no_delay = true`
   - Configures socket buffer sizes to `64 KB`
   - Configures application-level receive buffer size to `8 KB`
   - Implements short timeouts (e.g. `15s` idle timeout)

### Configuring the Default Profile

You can apply a default profile to `TcpListener` that will be inherited by all newly accepted client sessions. If you call legacy configuration methods (like `.SetNoDelay()`), the default connection profile will be updated automatically in sync.

```cpp
cpptcpnet::TcpListener server(8080);

// Use a preset
cpptcpnet::ConnectionProfile profile = cpptcpnet::ConnectionProfile::ReliableLAN();

// Or configure custom settings:
profile.no_delay = true;
profile.recv_buffer_size = 9999;
profile.idle_timeout = std::chrono::seconds(45);

// Set the default profile for all future connections
server.SetDefaultConnectionProfile(profile);

// Retrieve the default profile
cpptcpnet::ConnectionProfile current_default = server.GetDefaultConnectionProfile();
```

### Dynamic Profile Application

You can also dynamically apply a profile on the fly to a specific client connection. This allows your application to change settings based on client type, telemetry, or network conditions without disrupting other connections or restarting the server.

```cpp
// 1. Get active sessions
std::vector<uint64_t> sessions = server.GetActiveSessions();

if (!sessions.empty()) {
    uint64_t target_session = sessions[0];

    // 2. Query the current active profile for this session
    cpptcpnet::ConnectionProfile current_prof = server.GetConnectionProfile(target_session);

    // 3. Define a new profile (e.g. adjust idle timeout to 200ms)
    cpptcpnet::ConnectionProfile updated_prof = current_prof;
    updated_prof.idle_timeout = std::chrono::milliseconds(200);

    // 4. Apply the updated profile dynamically to that connection
    server.ApplyConnectionProfile(target_session, updated_prof);
}
```
 
## SSL/TLS Encryption

To secure communications over public or untrusted networks, `TcpListener` provides optional built-in SSL/TLS support. You must configure the listener with a certificate chain file and a private key file before starting it.

```cpp
#ifdef CPPTCPNET_SSL_SUPPORT
    cpptcpnet::TcpListener::SslConfig ssl_config;
    ssl_config.cert_file = "path/to/server.crt";
    ssl_config.key_file = "path/to/server.key";
    
    // Optional configuration settings:
    ssl_config.ca_file = "path/to/ca.crt";          // CA certificate path for client verification
    ssl_config.require_client_cert = false;         // Set to true to require and verify client certs
    ssl_config.verify_depth = -1;                   // Max certificate verification depth (-1 for default)
    ssl_config.cipher_list = "HIGH:!aNULL:!MD5";    // Custom cipher suites list (TLS <= 1.2)
    ssl_config.cipher_suites = "TLS_AES_256_GCM_SHA384"; // Custom cipher suites (TLS 1.3)
    ssl_config.min_tls_version = -1;                // Minimum TLS version (e.g. OpenSSL TLS1_2_VERSION constant)
    ssl_config.max_tls_version = -1;                // Maximum TLS version (e.g. OpenSSL TLS1_3_VERSION constant)
    
    // Enable SSL before starting the server
    server.EnableSSL(ssl_config);
#endif
```

> [!NOTE]
> The library automatically configures the SSL engine to enforce **TLS 1.2 or higher** as the minimum protocol version, ensuring that weak, deprecated TLS/SSL protocols are rejected.

## Security Warning (Plaintext Default)
 
> [!WARNING]
> By default, `cpp-tcpnet` transmits data in plaintext (cleartext) over raw TCP. Do not transmit sensitive, confidential, or authentication credentials over public or untrusted networks without enabling SSL/TLS via `.EnableSSL()`.
 
---
[< Previous: Getting Started](./getting-started.html) | [🏠 Home](./) | [Next: Advanced Usage >](./advanced-usage.html)
