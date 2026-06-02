---
layout: default
---

[< Previous: Advanced Usage](./advanced-usage.html) | [🏠 Home](./) | [Next: Architecture & Examples >](./architecture-and-examples.html)
<hr>

# Performance Metrics and Throughput Tracking

`cpp-tcpnet` includes built-in support for monitoring performance, allowing you to track data transfer volumes, packet counts, active connection counts, and real-time throughput.

The library offers two complementary tracking models:
1. **Direct Stats (Atomic Counters)**: Low-overhead, synchronous snapshots of cumulative counts.
2. **Throughput Tracking (PubSub Event-Driven)**: An asynchronous sliding-window tracker powered by a background worker.

---

## 1. Direct Stats (Atomic Counters)

Both `TcpListener` and `TcpClient` maintain atomic counters of data transmitted and received. These stats can be queried synchronously at any time using the `.GetStats()` method.

### Listener Stats
`TcpListener::GetStats()` returns a `ListenerStats` struct:
* `bytes_sent`: Total bytes successfully sent across all client sessions.
* `bytes_received`: Total bytes successfully read from all client sessions.
* `packets_sent`: Total number of individual socket writes.
* `packets_received`: Total number of individual socket reads.
* `active_connections`: Number of client sessions currently connected.
* `total_connections`: Total connections accepted since the server started.

**Example usage:**
```cpp
cpptcpnet::ListenerStats stats = server.GetStats();
std::cout << "Active Clients: " << stats.active_connections << "\n"
          << "Total Received: " << stats.bytes_received << " bytes ("
          << stats.packets_received << " reads)\n";
```

### Client Stats
`TcpClient::GetStats()` returns a `ClientStats` struct:
* `bytes_sent`: Total bytes successfully sent to the server.
* `bytes_received`: Total bytes successfully read from the server.
* `packets_sent`: Total number of individual socket writes.
* `packets_received`: Total number of individual socket reads.
* `is_connected`: Whether the client is currently connected.

---

## 2. Event-Driven Throughput Tracking

Whenever data is sent or received, the network loops publish a `TransferEvent` onto the `"transfer_events"` topic of the internal PubSub broker.

```cpp
struct TransferEvent {
    uint64_t session_id;
    size_t bytes_transferred;
    bool is_send; // true if sent, false if received
};
```

You can subscribe to these events directly using the broker to implement custom metrics collection.

### Using the `ThroughputTracker` Utility

`cpp-tcpnet` provides a built-in `ThroughputTracker` utility class that automatically monitors this event stream and computes a sliding-window data rate (bytes per second).

**Example usage:**
```cpp
#include "cpptcpnet.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    cpptcpnet::TcpListener server(8080);
    server.Start();

    // Create a tracker with a 1-second sliding window
    cpptcpnet::ThroughputTracker tracker(server.GetEventBroker(), std::chrono::seconds(1));

    // Monitor throughput in a loop (assuming server.IsRunning() or similar loop condition)
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "Upload rate: " << tracker.GetSendThroughputBytesPerSec() << " B/s | "
                  << "Download rate: " << tracker.GetRecvThroughputBytesPerSec() << " B/s\n";
    }

    server.Stop();
    return 0;
}
```

---

## 3. Throughput Scaling Utilities

`cpp-tcpnet` provides formatting helper functions to scale raw byte counts or bit rates into human-readable strings with binary (KB, MB, GB, etc.) or decimal (Kbps, Mbps, Gbps, etc.) units.

* `cpptcpnet::ScaleBytes(uint64_t bytes)`: Scales a byte count to its most appropriate binary unit (B, KB, MB, GB, TB). Returns a `ScaledUnit` struct.
* `cpptcpnet::ScaleBits(double bits_per_sec)`: Scales a bit rate (bits per second) to its most appropriate decimal unit (bps, Kbps, Mbps, Gbps, Tbps). Returns a `ScaledUnit` struct.

### `ScaledUnit` Structure
Both utilities return a `ScaledUnit` struct containing:
* `value` (double): The scaled numeric value.
* `unit` (const char*): The corresponding unit label string.

**Example usage:**
```cpp
uint64_t bytes_transferred = 5368709120; // 5 GB
cpptcpnet::ScaledUnit scaled = cpptcpnet::ScaleBytes(bytes_transferred);
std::cout << "Transferred: " << scaled.value << " " << scaled.unit << "\n"; // Outputs: Transferred: 5 GB

double bit_rate = 12500000; // 12.5 Mbps
cpptcpnet::ScaledUnit scaled_rate = cpptcpnet::ScaleBits(bit_rate);
std::cout << "Speed: " << scaled_rate.value << " " << scaled_rate.unit << "\n"; // Outputs: Speed: 12.5 Mbps
```

---
[< Previous: Advanced Usage](./advanced-usage.html) | [🏠 Home](./) | [Next: Architecture & Examples >](./architecture-and-examples.html)
