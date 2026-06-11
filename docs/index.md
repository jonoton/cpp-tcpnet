---
layout: default
---

# cpp-tcpnet

`cpp-tcpnet` is a robust, cross-platform, header-only C++ library for TCP client and server networking. It uses `cpp-pubsub` for event dispatching and provides easy-to-use, non-blocking networking capabilities.

### Key Features
- **Cross-Platform:** Works natively on Windows (`Winsock2`) and Linux/macOS (`POSIX`).
- **DNS Resolution & IPv6:** Resolve hostnames dynamically on client connections. Fully supports both IPv4 and IPv6 (`AF_INET6`), including dual-stack wildcard binding.
- **SSL/TLS Support:** Secure encrypted transport using OpenSSL/LibreSSL (TLS 1.2+).
- **Asynchronous & Non-Blocking:** High-performance background event loop ensures the main thread is never stalled by network IO.
- **Built-in Thread Pool with Session Affinity:** Automatically dispatches incoming packet data to a background worker pool, guaranteeing that callbacks for any single session are executed serially (in-order), while different sessions are processed concurrently.
- **Event-Driven:** Uses the `cpp-pubsub` broker to publish events whenever connections connect or disconnect.
- **Peer Address API:** Retrieve the remote peer's IP address and port for any active session with `GetPeerAddress(session_id)`.
- **Zero-Copy Transmissions:** Safely bypass memory allocations and data copying during transmission using C++17 move semantics (`std::move`) or shared reference counts (`std::shared_ptr<const T>`).
- **Dynamic Connection Profiles:** Package socket options and application network configurations into preset or custom `ConnectionProfile` objects and apply them dynamically on the fly per session.
- **Highly Configurable:** Fine-tune socket options (Nagle's algorithm, keepalives, linger, reuse port), thread pool bounds, outbound buffer limits, dynamically allocated receive buffers, custom cipher suites, TLS version bounds, idle timeouts, and automatic client reconnection with backoff.
- **Performance Metrics:** Monitor cumulative bytes/packets sent and received, connection counts, calculate real-time throughput using the sliding-window `ThroughputTracker`, and scale raw counts using formatting helpers.

## Documentation Pages

Welcome to the `cpp-tcpnet` documentation! Please follow the guide below to learn how to integrate and use the library:

1. **[Getting Started](./getting-started.html)**
   Learn how to integrate `cpp-tcpnet` into your project via CMake or direct inclusion.
   
2. **[Basic Usage](./basic-usage.html)**
   Learn how to start a TCP server and handle incoming connections.

3. **[Advanced Usage](./advanced-usage.html)**
   Learn how to connect a TCP client and handle asynchronous messaging.

4. **[Performance Metrics](./performance-metrics.html)**
   Learn how to monitor network statistics and track real-time throughput.

5. **[Architecture & Examples](./architecture-and-examples.html)**
   Understand the architecture behind the library and its multi-threaded event loop.

---
[Start Reading: Getting Started >](./getting-started.html)
