---
layout: default
---

[< Previous: Basic Usage](./basic-usage.html) | [🏠 Home](./) | [Next: Performance Metrics >](./performance-metrics.html)
<hr>

# Advanced Usage (Client)

The `TcpClient` class manages non-blocking outbound TCP connections. It provides the same threaded data handler logic and pubsub state events as the server.

## Connecting a Client

```cpp
#include "cpptcpnet.hpp"
#include <iostream>

int main() {
    cpptcpnet::TcpClient client;

    // Set a handler to process data received from the server
    client.SetDataHandler([](uint64_t session_id, const std::vector<uint8_t>& data) {
        std::string text(data.begin(), data.end());
        std::cout << "Server says: " << text << std::endl;
    });

    // Start the client background thread
    client.Start();

    // Connect to the server. Supports IPv4, IPv6, and hostnames (e.g., "localhost", "example.com").
    try {
        client.Connect("localhost", 8080);
        // Since there is exactly one active connection, we can omit the session ID
        client.Send("Hello Server!\n");
        
        std::cout << "Connected! Press Enter to exit." << std::endl;
        std::cin.get();
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect: " << e.what() << std::endl;
    }

    client.Stop();
    return 0;
}
```

## Lifecycle Events

Just like the server, the client emits `state_events` on its internal broker when the connection is established or lost.

```cpp
cpppubsub::Worker worker;
auto sub = client.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");

worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [](const cpptcpnet::ConnectionEvent& event) {
    if (event.state == cpptcpnet::ConnectionState::Disconnected) {
        std::cout << "Connection to server lost." << std::endl;
    }
});

// Start the worker thread
worker.Start();
```

## Error Handling

`TcpClient` splits error handling into synchronous and asynchronous categories:

1. **Synchronous Errors**: Methods like `Start()` and `Connect()` throw `std::system_error` or `std::runtime_error` on failure.
2. **Asynchronous Errors**: Background connection dropouts, polling errors, or TLS handshake failures are published to the `"error_events"` topic on the event broker, or can be caught via a direct callback registered with `SetErrorHandler`.

```cpp
// Set a direct error handler callback
client.SetErrorHandler([](int error_code, const std::string& message) {
    std::cerr << "Client background error [" << error_code << "]: " << message << std::endl;
});
```

## Zero-Copy Transmissions

For high-throughput workloads, `cpp-tcpnet` supports zero-copy sending overloads of the `.Send()` method. By default, calling `.Send()` with a `const std::vector<uint8_t>&` or `const std::string&` copies the data into the library's internal outbound queue. The zero-copy overloads avoid memory allocation and copying entirely, utilizing C++17 move semantics or reference-counted shared pointers instead.

These zero-copy methods are available on both `TcpClient` and `TcpListener`.

### 1. Move-Based Zero-Copy (`std::move`)

When you pass an rvalue reference (using `std::move`), the outbound queue takes ownership of the container's internal data array. The original local variable is left empty.

```cpp
// Moving a vector
std::vector<uint8_t> payload = { 'D', 'a', 't', 'a' };
client.Send(std::move(payload)); 
// payload is now empty; data was transferred to the outbound queue without copying

// Moving a string
std::string text = "A large text payload...";
client.Send(std::move(text)); 
// text is now empty
```

### 2. Shared Pointer-Based Zero-Copy (`std::shared_ptr`)

If you want to send the same read-only buffer to multiple sessions, or if the caller needs to keep a reference to the data, you can pass a `std::shared_ptr<const std::vector<uint8_t>>` or `std::shared_ptr<const std::string>`.

The outbound queue stores a copy of the `std::shared_ptr`, incrementing its reference count. Once the background polling thread has successfully written all of the chunk's data to the socket, the chunk is popped from the queue and the reference count is decremented.

```cpp
// Define shared data
auto shared_payload = std::make_shared<const std::string>("shared immutable content");

// Send to multiple active sessions (or the single client connection)
client.Send(shared_payload); // use_count increases to 2 (1 in caller, 1 in outbound queue)

// Once the background worker finishes writing it to the socket and drains the queue,
// the use count automatically decreases back to 1.
```

## Client Configuration & Socket Settings

`TcpClient` provides a set of configuration settings to control connection timeouts, socket-level settings, threading pool size, and connection recovery parameters:

### Connect, Send, and Idle Timeouts
* **Connect Timeout**: The maximum duration to wait for an asynchronous connection to establish.
* **Send Timeout**: Drops the server connection if pending outbound data remains unflushed.
* **Idle Timeout**: Drops the server connection if no read or write activity is detected for longer than the specified timeout window.
* **SSL Handshake Timeout**: Limits the duration permitted to complete the TLS handshake on secure connections.

```cpp
client.SetConnectTimeout(std::chrono::seconds(10));      // Default: 10s
client.SetSendTimeout(std::chrono::seconds(30));         // Default: 30s
client.SetIdleTimeout(std::chrono::seconds(60));         // Default: 60s (0/disabled by default)
client.SetSslHandshakeTimeout(std::chrono::seconds(10)); // Default: 10s
```

### Auto-Reconnect
If the connection is lost due to network dropouts or server restart, `TcpClient` can automatically attempt reconnection using an exponential backoff strategy:
* **Auto-Reconnect**: Toggles automatic connection recovery.
* **Backoff Configuration**: Sets the initial delay and the maximum limit delay.

```cpp
// Enable auto-reconnect: start with 1s delay, back off up to 30s max
client.SetAutoReconnect(true, std::chrono::seconds(1), std::chrono::seconds(30));
```

### Socket-Level Tuning
* **TCP_NODELAY**: Controls Nagle's algorithm (latency-focused coalescing).
* **SO_RCVBUF / SO_SNDBUF**: Configures the kernel socket receive/send buffer sizes.
* **SO_KEEPALIVE Tuning**: Enables and configures keepalive probes.
* **SO_LINGER**: Configures socket linger behavior on close.

```cpp
client.SetNoDelay(true); // Disable Nagle's algorithm

// Set kernel socket buffers
client.SetSocketRecvBufferSize(65536);
client.SetSocketSendBufferSize(65536);

// Tune Keep-Alives
cpptcpnet::KeepAliveConfig ka_config;
ka_config.enabled = true;
ka_config.idle_secs = 30;
ka_config.interval_secs = 10;
ka_config.count = 3;
client.SetKeepAliveConfig(ka_config);

// Set Linger on Close
client.SetLinger(true, 5);
```

### Threading & Buffers
* **Worker Thread Count**: The size of the thread pool processing data and error callbacks.
* **Receive Buffer Size**: The application-level receive buffer size.
* **Send Chunk Size**: The maximum segment size transmitted per socket send operation.
* **Max Outbound Buffer Size**: The maximum accumulated outbound buffer size (in bytes) allowed. Defaults to 10MB (`10 * 1024 * 1024`). If queued send data exceeds this, further writes fail.

```cpp
client.SetWorkerThreadCount(4);  // Use 4 worker threads
client.SetRecvBufferSize(16384); // 16KB read buffer
client.SetSendChunkSize(32768);  // 32KB send chunks
client.SetMaxOutboundBufferSize(50 * 1024 * 1024); // 50MB limit
```

### Querying Configuration at Runtime
All configuration settings have corresponding getter methods to query current values at runtime. For example:
```cpp
bool no_delay = client.GetNoDelay();
size_t max_buf = client.GetMaxOutboundBufferSize();
std::chrono::milliseconds idle = client.GetIdleTimeout();
bool auto_reconnect = client.GetAutoReconnectEnabled();
```

### Connection Management
You can programmatically retrieve the server's IP and port, disconnect the active connection, check if the client is running, or query the list of active session IDs:
```cpp
if (client.IsRunning()) {
    std::vector<uint64_t> sessions = client.GetActiveSessions();
    for (uint64_t session_id : sessions) {
        // Retrieve the remote server's peer address (IP and port)
        try {
            cpptcpnet::PeerAddress peer = client.GetPeerAddress(session_id);
            std::cout << "Session: " << session_id << " connected to " << peer.ip << ":" << peer.port << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Failed to query peer address: " << e.what() << std::endl;
        }

        // Disconnect a connection
        client.Disconnect(session_id);
    }
}
```

## Connection Profiles

`TcpClient` fully supports the `ConnectionProfile` system. You can configure a client's default connection options, and dynamically modify socket properties on the fly.

### Setting a Default Profile

Setting a default profile applies those network parameters to all future connections initiated by the client:

```cpp
cpptcpnet::TcpClient client;

// Choose from standard presets: HighLatency(), LowBandwidth(), ReliableLAN()
cpptcpnet::ConnectionProfile profile = cpptcpnet::ConnectionProfile::HighLatency();
profile.connect_timeout = std::chrono::seconds(15);

// Set default profile for future connections
client.SetDefaultConnectionProfile(profile);
```

### Applying a Profile to Active Connections

You can apply a new profile to an active connection dynamically on the fly. On the client side, if there is exactly one active connection, you can omit the session ID:

```cpp
// Option A: Apply on the fly to a specific session ID
uint64_t session_id = ...;
client.ApplyConnectionProfile(session_id, profile);

// Option B: Apply to the single active connection (throws if multiple connections are active)
client.ApplyConnectionProfile(profile);

// Retrieve the current profile for a session
cpptcpnet::ConnectionProfile current_prof = client.GetConnectionProfile(session_id);
```
 
## SSL/TLS Encryption

To secure client connections, call `EnableSSL()` before starting or connecting the client. You can call it without arguments to use default SSL settings, or pass a custom `SslClientConfig` struct:

```cpp
#ifdef CPPTCPNET_SSL_SUPPORT
    cpptcpnet::TcpClient::SslClientConfig ssl_config;
    ssl_config.ca_file = "path/to/ca.crt";              // CA certificate file path to verify server
    ssl_config.ca_path = "path/to/ca_directory";        // CA directory path to verify server
    ssl_config.verify_peer = true;                      // Enable server certificate verification (default: false)
    ssl_config.client_cert_file = "path/to/client.crt"; // Optional: client certificate for mutual TLS (mTLS)
    ssl_config.client_key_file = "path/to/client.key";   // Optional: client private key for mutual TLS (mTLS)
    ssl_config.cipher_list = "HIGH:!aNULL:!MD5";        // Custom cipher suites list (TLS <= 1.2)
    ssl_config.cipher_suites = "TLS_AES_256_GCM_SHA384"; // Custom cipher suites (TLS 1.3)
    ssl_config.min_tls_version = -1;                    // Minimum TLS version (e.g. OpenSSL TLS1_2_VERSION constant)
    ssl_config.max_tls_version = -1;                    // Maximum TLS version (e.g. OpenSSL TLS1_3_VERSION constant)

    // Enable SSL with custom configuration
    client.EnableSSL(ssl_config);

    // Or enable SSL with default configurations:
    // client.EnableSSL();
#endif
```

> [!NOTE]
> The library automatically configures the SSL engine to enforce **TLS 1.2 or higher** as the minimum protocol version, ensuring that weak, deprecated TLS/SSL protocols are rejected.

## Security Warning (Plaintext Default)
 
> [!WARNING]
> By default, `TcpClient` transmits data in plaintext (cleartext) over raw TCP. Do not transmit sensitive, confidential, or authentication credentials over public or untrusted networks without enabling SSL/TLS via `.EnableSSL()`.
 
---
[< Previous: Basic Usage](./basic-usage.html) | [🏠 Home](./) | [Next: Performance Metrics >](./performance-metrics.html)
