---
layout: default
---

[< Previous: Performance Metrics](./performance-metrics.html) | [🏠 Home](./)
<hr>

# Architecture & Examples

## Threading Model

The `cpp-tcpnet` library heavily utilizes multi-threading to ensure that network IO never blocks your main application logic.

1. **Poll Loop Thread**: Both `TcpListener` and `TcpClient` spawn a dedicated background thread that uses `poll()` (or `WSAPoll` on Windows) to monitor socket activity non-blockingly.
2. **Worker Pools (with Session Affinity)**: When data arrives on a socket, it is read and dispatched to one of several single-threaded worker pools (`cppasyncworker::WorkerPool`) based on a hash of the session ID. This ensures **session affinity**—meaning all callbacks for a specific session are executed sequentially and in-order, preventing race conditions within a session handler. Meanwhile, callbacks for different sessions are distributed across the worker pool threads to execute concurrently.
3. **Event Broker**: State changes (like connection or disconnection) are published asynchronously via `cpppubsub::PubSub`, allowing any part of your application to react to network lifecycle events safely.

## Dependencies

This library relies on two sibling projects:
- [cpp-pubsub](https://github.com/jonoton/cpp-pubsub) for event routing.
- [cpp-asyncworker](https://github.com/jonoton/cpp-asyncworker) for thread pooling.

---
[< Previous: Performance Metrics](./performance-metrics.html) | [🏠 Home](./)
