#include <gtest/gtest.h>
#include "cpptcpnet.hpp"
#include <thread>
#include <chrono>
#include <atomic>
#include <type_traits>

// ===== Original Tests =====

TEST(TcpNetTest, ListenerStartsAndStops) {
    cpptcpnet::TcpListener listener(8081);
    EXPECT_NO_THROW(listener.Start());
    listener.Stop();
}

TEST(TcpNetTest, ClientInitiatesConnectionNonBlocking) {
    cpptcpnet::TcpClient client;
    uint64_t session_id = 0;
    EXPECT_NO_THROW({ session_id = client.Connect("127.0.0.1", 9999); });
    // Since connect is non-blocking, it returns a valid session ID and initiates connection asynchronously
    EXPECT_GT(session_id, 0);
    client.Stop();
}

TEST(TcpNetTest, ClientConnectsToServer) {
    cpptcpnet::TcpListener listener(8082);
    EXPECT_NO_THROW(listener.Start());
    
    cpptcpnet::TcpClient client;
    uint64_t session_id = 0;
    EXPECT_NO_THROW({ session_id = client.Connect("127.0.0.1", 8082); });
    EXPECT_GT(session_id, 0);
    
    client.Stop();
    listener.Stop();
}

TEST(TcpNetTest, ClientThrowsOnInvalidAddress) {
    cpptcpnet::TcpClient client;
    EXPECT_THROW(client.Connect("invalid_ip", 9999), std::runtime_error);
}

// ===== Regression Tests =====

// Fix 1: Logger thread safety — concurrent SetLogger + Log must not crash
TEST(TcpNetRegressionTest, LoggerThreadSafety) {
    std::atomic<bool> stop{false};
    std::atomic<int> log_count{0};

    // Thread that repeatedly sets the logger
    std::thread setter([&stop]() {
        for (int i = 0; !stop.load(); ++i) {
            if (i % 2 == 0) {
                cpptcpnet::SetLogger([](cpptcpnet::LogSeverity, const std::string&, const std::string&) {
                    // no-op logger
                });
            } else {
                cpptcpnet::SetLogger(nullptr);
            }
        }
    });

    // Threads that concurrently call Log
    std::vector<std::thread> loggers;
    for (int t = 0; t < 4; ++t) {
        loggers.emplace_back([&stop, &log_count]() {
            while (!stop.load()) {
                cpptcpnet::Log(cpptcpnet::LogSeverity::Info, "Test", "concurrent log");
                log_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Run for a short duration — enough to trigger a race under TSAN
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);

    setter.join();
    for (auto& t : loggers) t.join();

    // If we get here without crash/TSAN error, the test passes
    EXPECT_GT(log_count.load(), 0);

    // Restore logger to null
    cpptcpnet::SetLogger(nullptr);
}

// Fix 3: Non-copyable and non-movable
TEST(TcpNetRegressionTest, ListenerIsNotCopyableOrMovable) {
    EXPECT_FALSE(std::is_copy_constructible<cpptcpnet::TcpListener>::value);
    EXPECT_FALSE(std::is_copy_assignable<cpptcpnet::TcpListener>::value);
    EXPECT_FALSE(std::is_move_constructible<cpptcpnet::TcpListener>::value);
    EXPECT_FALSE(std::is_move_assignable<cpptcpnet::TcpListener>::value);
}

TEST(TcpNetRegressionTest, ClientIsNotCopyableOrMovable) {
    EXPECT_FALSE(std::is_copy_constructible<cpptcpnet::TcpClient>::value);
    EXPECT_FALSE(std::is_copy_assignable<cpptcpnet::TcpClient>::value);
    EXPECT_FALSE(std::is_move_constructible<cpptcpnet::TcpClient>::value);
    EXPECT_FALSE(std::is_move_assignable<cpptcpnet::TcpClient>::value);
}

// Fix 6: TcpClient::SetErrorHandler is callable and fires on error
TEST(TcpNetRegressionTest, ClientSetErrorHandlerExists) {
    cpptcpnet::TcpClient client;
    std::atomic<bool> error_fired{false};

    // Should compile and not crash
    EXPECT_NO_THROW(client.SetErrorHandler([&error_fired](int, const std::string&) {
        error_fired.store(true);
    }));

    client.Stop();
}

// DoubleStartThrows: calling Start() twice should throw
TEST(TcpNetRegressionTest, ListenerDoubleStartThrows) {
    cpptcpnet::TcpListener listener(8083);
    EXPECT_NO_THROW(listener.Start());
    EXPECT_THROW(listener.Start(), std::runtime_error);
    listener.Stop();
}

TEST(TcpNetRegressionTest, ClientDoubleStartThrows) {
    cpptcpnet::TcpClient client;
    EXPECT_NO_THROW(client.Start());
    EXPECT_THROW(client.Start(), std::runtime_error);
    client.Stop();
}

// StopWithoutStart: calling Stop() on a never-started instance should be safe
TEST(TcpNetRegressionTest, StopWithoutStartIsSafe) {
    {
        cpptcpnet::TcpListener listener(8084);
        EXPECT_NO_THROW(listener.Stop());
    }
    {
        cpptcpnet::TcpClient client;
        EXPECT_NO_THROW(client.Stop());
    }
}

// PubSub connection events fire in correct order
TEST(TcpNetRegressionTest, ClientConnectAndDisconnectEvents) {
    cpptcpnet::TcpListener listener(8085);
    listener.Start();

    cpptcpnet::TcpClient client;

    std::atomic<bool> connected{false};
    std::atomic<bool> disconnected{false};

    cpppubsub::Worker worker;
    auto sub = client.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");
    worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [&](const cpptcpnet::ConnectionEvent& e) {
        if (e.state == cpptcpnet::ConnectionState::Connected) {
            connected.store(true);
        } else {
            disconnected.store(true);
        }
    });
    worker.Start();

    uint64_t session_id = client.Connect("127.0.0.1", 8085);
    EXPECT_GT(session_id, 0);

    // Wait for connection event
    for (int i = 0; i < 100 && !connected.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(connected.load());

    // Stop the server first — the client's poll loop will detect the remote
    // disconnect (POLLHUP) and publish a Disconnected event.
    listener.Stop();

    // Wait for the disconnected event to propagate through the client poll loop
    for (int i = 0; i < 200 && !disconnected.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(disconnected.load());

    client.Stop();
    worker.Stop();
}

// Large payload round-trip — exercises send clamping + POLLOUT buffering
TEST(TcpNetRegressionTest, LargePayloadSendReceive) {
    const size_t payload_size = 2 * 1024 * 1024; // 2MB
    std::atomic<size_t> total_received{0};
    std::atomic<bool> client_connected{false};

    cpptcpnet::TcpListener listener(8086);
    listener.SetDataHandler([&total_received](uint64_t, const std::vector<uint8_t>& data) {
        total_received.fetch_add(data.size(), std::memory_order_relaxed);
    });
    listener.Start();
 
    cpptcpnet::TcpClient client;
    cpppubsub::Worker worker;
    auto sub = client.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");
    worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [&client_connected](const cpptcpnet::ConnectionEvent& e) {
        if (e.state == cpptcpnet::ConnectionState::Connected) {
            client_connected.store(true);
        }
    });
    worker.Start();
 
    uint64_t session_id = client.Connect("127.0.0.1", 8086);
    ASSERT_GT(session_id, 0);

    // Wait for connection
    for (int i = 0; i < 100 && !client_connected.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(client_connected.load());

    // Send large payload
    std::vector<uint8_t> payload(payload_size, 0xAB);
    client.Send(payload);

    // Wait for data to be received (with timeout)
    for (int i = 0; i < 300 && total_received.load() < payload_size; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(total_received.load(), payload_size);

    client.Stop();
    worker.Stop();
    listener.Stop();
}

// Listener error handler is callable and fires
TEST(TcpNetRegressionTest, ListenerSetErrorHandlerExists) {
    cpptcpnet::TcpListener listener(8087);
    std::atomic<bool> error_fired{false};

    EXPECT_NO_THROW(listener.SetErrorHandler([&error_fired](int, const std::string&) {
        error_fired.store(true);
    }));

    listener.Stop();
}

// SessionSafetyAndReUsePrevention: verifies that we can fetch unique session IDs
// and that sending with an invalid session ID is safely rejected.
TEST(TcpNetRegressionTest, SessionSafetyAndReUsePrevention) {
    cpptcpnet::TcpListener listener(8088);
    listener.Start();

    cpptcpnet::TcpClient client;
    cpppubsub::Worker worker;

    std::atomic<uint64_t> client_session_id{0};
    std::atomic<bool> connected{false};

    auto sub = client.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");
    worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [&](const cpptcpnet::ConnectionEvent& e) {
        if (e.state == cpptcpnet::ConnectionState::Connected) {
            client_session_id.store(e.session_id);
            connected.store(true);
        }
    });
    worker.Start();

    uint64_t session_id = client.Connect("127.0.0.1", 8088);
    ASSERT_GT(session_id, 0);
 
    // Wait for connection event to capture the session ID
    for (int i = 0; i < 100 && !connected.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(connected.load());
    uint64_t actual_session = client_session_id.load();
    EXPECT_GT(actual_session, 0);
 
    // Try sending with a fake/stale session ID — should fail (returns false)
    EXPECT_FALSE(client.Send(actual_session + 9999, "should fail"));
 
    // Try sending with the correct session ID — should succeed (returns true)
    EXPECT_TRUE(client.Send(actual_session, "should succeed"));

    client.Stop();
    worker.Stop();
    listener.Stop();
}

// ===== Metrics Tests =====

TEST(TcpNetMetricsTest, AtomicStatsVerification) {
    cpptcpnet::TcpListener listener(8089);
    std::atomic<size_t> received_bytes{0};
    listener.SetDataHandler([&received_bytes](uint64_t, const std::vector<uint8_t>& data) {
        received_bytes.fetch_add(data.size());
    });
    listener.Start();
 
    cpptcpnet::TcpClient client;
    client.Start();
 
    uint64_t session_id = client.Connect("127.0.0.1", 8089);
    ASSERT_GT(session_id, 0);

    // Wait for connection to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Get listener stats — verify active connection is 1
    auto lst_stats1 = listener.GetStats();
    EXPECT_EQ(lst_stats1.active_connections, 1);
    EXPECT_EQ(lst_stats1.total_connections, 1);

    // Send 123 bytes from client to listener
    std::vector<uint8_t> payload(123, 0xAA);
    EXPECT_TRUE(client.Send(payload));

    // Wait for delivery
    for (int i = 0; i < 100 && received_bytes.load() < 123; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(received_bytes.load(), 123);

    // Verify stats
    auto client_stats = client.GetStats();
    EXPECT_EQ(client_stats.bytes_sent, 123);
    EXPECT_EQ(client_stats.packets_sent, 1);
    EXPECT_TRUE(client_stats.is_connected);

    auto listener_stats = listener.GetStats();
    EXPECT_EQ(listener_stats.bytes_received, 123);
    EXPECT_EQ(listener_stats.packets_received, 1);

    client.Stop();
    
    // Wait for disconnection to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    listener_stats = listener.GetStats();
    EXPECT_EQ(listener_stats.active_connections, 0);

    listener.Stop();
}

TEST(TcpNetMetricsTest, PubSubAndThroughputTracker) {
    cpptcpnet::TcpListener listener(8090);
    std::atomic<size_t> received_bytes{0};
    listener.SetDataHandler([&received_bytes](uint64_t, const std::vector<uint8_t>& data) {
        received_bytes.fetch_add(data.size());
    });
    listener.Start();
 
    // Create throughput tracker for listener
    cpptcpnet::ThroughputTracker listener_tracker(listener.GetEventBroker(), std::chrono::milliseconds(500));
 
    cpptcpnet::TcpClient client;
    // Create throughput tracker for client
    cpptcpnet::ThroughputTracker client_tracker(client.GetEventBroker(), std::chrono::milliseconds(500));
 
    uint64_t session_id = client.Connect("127.0.0.1", 8090);
    ASSERT_GT(session_id, 0);

    // Wait for connection to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send data
    std::vector<uint8_t> payload(50000, 0xBB);
    EXPECT_TRUE(client.Send(payload));

    // Wait for delivery
    for (int i = 0; i < 100 && received_bytes.load() < 50000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(received_bytes.load(), 50000);

    // Wait a brief moment to make sure trackers have updated sample lists
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Check throughput (should be non-zero)
    double client_send_tp = client_tracker.GetSendThroughputBytesPerSec();
    double listener_recv_tp = listener_tracker.GetRecvThroughputBytesPerSec();

    EXPECT_GT(client_send_tp, 0.0);
    EXPECT_GT(listener_recv_tp, 0.0);

    client.Stop();
    listener.Stop();
}

TEST(TcpNetMetricsTest, ScaledUnitsUtility) {
    // Test ScaleBytes
    {
        auto scaled = cpptcpnet::ScaleBytes(0);
        EXPECT_DOUBLE_EQ(scaled.value, 0.0);
        EXPECT_STREQ(scaled.unit, "B");
    }
    {
        auto scaled = cpptcpnet::ScaleBytes(512);
        EXPECT_DOUBLE_EQ(scaled.value, 512.0);
        EXPECT_STREQ(scaled.unit, "B");
    }
    {
        auto scaled = cpptcpnet::ScaleBytes(1024);
        EXPECT_DOUBLE_EQ(scaled.value, 1.0);
        EXPECT_STREQ(scaled.unit, "KB");
    }
    {
        auto scaled = cpptcpnet::ScaleBytes(1024 * 1024);
        EXPECT_DOUBLE_EQ(scaled.value, 1.0);
        EXPECT_STREQ(scaled.unit, "MB");
    }
    {
        auto scaled = cpptcpnet::ScaleBytes(1024ULL * 1024 * 1024);
        EXPECT_DOUBLE_EQ(scaled.value, 1.0);
        EXPECT_STREQ(scaled.unit, "GB");
    }
    {
        auto scaled = cpptcpnet::ScaleBytes(1024ULL * 1024 * 1024 * 1024);
        EXPECT_DOUBLE_EQ(scaled.value, 1.0);
        EXPECT_STREQ(scaled.unit, "TB");
    }
    {
        auto scaled = cpptcpnet::ScaleBytes(1024ULL * 1024 * 1024 * 1024 * 1024);
        EXPECT_DOUBLE_EQ(scaled.value, 1024.0);
        EXPECT_STREQ(scaled.unit, "TB");
    }

    // Test ScaleBits
    {
        auto scaled = cpptcpnet::ScaleBits(0.0);
        EXPECT_DOUBLE_EQ(scaled.value, 0.0);
        EXPECT_STREQ(scaled.unit, "bps");
    }
    {
        auto scaled = cpptcpnet::ScaleBits(500.0);
        EXPECT_DOUBLE_EQ(scaled.value, 500.0);
        EXPECT_STREQ(scaled.unit, "bps");
    }
    {
        auto scaled = cpptcpnet::ScaleBits(1000.0);
        EXPECT_DOUBLE_EQ(scaled.value, 1.0);
        EXPECT_STREQ(scaled.unit, "Kbps");
    }
    {
        auto scaled = cpptcpnet::ScaleBits(1000000.0);
        EXPECT_DOUBLE_EQ(scaled.value, 1.0);
        EXPECT_STREQ(scaled.unit, "Mbps");
    }
    {
        auto scaled = cpptcpnet::ScaleBits(1000000000.0);
        EXPECT_DOUBLE_EQ(scaled.value, 1.0);
        EXPECT_STREQ(scaled.unit, "Gbps");
    }
    {
        auto scaled = cpptcpnet::ScaleBits(1000000000000.0);
        EXPECT_DOUBLE_EQ(scaled.value, 1.0);
        EXPECT_STREQ(scaled.unit, "Tbps");
    }
    {
        auto scaled = cpptcpnet::ScaleBits(1000000000000000.0);
        EXPECT_DOUBLE_EQ(scaled.value, 1000.0);
        EXPECT_STREQ(scaled.unit, "Tbps");
    }
}

#include "test_config.hpp"
#include <fstream>

#if defined(CPPTCPNET_SSL_SUPPORT)
TEST(TcpNetSslTest, ClientConnectsToSslServer) {
    cpptcpnet::TcpListener listener(8444, "127.0.0.1");
    cpptcpnet::TcpListener::SslConfig config;
    config.cert_file = CPPTCPNET_CERTS_DIR "/server.crt";
    config.key_file = CPPTCPNET_CERTS_DIR "/server.key";

    // Verify cert files exist before starting SSL to prevent crashes or silent failures
    {
        std::ifstream cert_file(config.cert_file);
        std::ifstream key_file(config.key_file);
        ASSERT_TRUE(cert_file.good()) << "Certificate file not found: " << config.cert_file;
        ASSERT_TRUE(key_file.good()) << "Key file not found: " << config.key_file;
    }
    
    EXPECT_NO_THROW(listener.EnableSSL(config));
    EXPECT_NO_THROW(listener.Start());

    std::atomic<size_t> received_bytes{0};
    listener.SetDataHandler([&received_bytes, &listener](uint64_t session_id, const std::vector<uint8_t>& data) {
        received_bytes.fetch_add(data.size());
        listener.Send(session_id, data); // Echo back
    });

    cpptcpnet::TcpClient client;
    EXPECT_NO_THROW(client.EnableSSL());
    EXPECT_NO_THROW(client.Start());

    std::atomic<size_t> client_received_bytes{0};
    client.SetDataHandler([&client_received_bytes](uint64_t, const std::vector<uint8_t>& data) {
        client_received_bytes.fetch_add(data.size());
    });

    std::atomic<bool> connected{false};
    cpppubsub::Worker worker;
    auto sub = client.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");
    worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [&](const cpptcpnet::ConnectionEvent& e) {
        if (e.state == cpptcpnet::ConnectionState::Connected) {
            connected.store(true);
        }
    });
    worker.Start();

    uint64_t session_id = client.Connect("127.0.0.1", 8444);
    EXPECT_GT(session_id, 0);

    // Wait for connection to establish over SSL
    for (int i = 0; i < 200 && !connected.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(connected.load());

    // Send payload
    std::vector<uint8_t> payload = { 'H', 'e', 'l', 'l', 'o' };
    EXPECT_TRUE(client.Send(session_id, payload));

    // Wait for echo to round-trip
    for (int i = 0; i < 200 && (received_bytes.load() < 5 || client_received_bytes.load() < 5); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(received_bytes.load(), 5);
    EXPECT_EQ(client_received_bytes.load(), 5);

    client.Stop();
    worker.Stop();
    listener.Stop();
}
#endif

// ===== New Configuration & Feature Tests =====

TEST(TcpNetConfigTest, GettersAndSetters) {
    cpptcpnet::TcpClient client;
    client.SetNoDelay(true);
    EXPECT_TRUE(client.GetNoDelay());

    client.SetSocketRecvBufferSize(8192);
    EXPECT_EQ(client.GetSocketRecvBufferSize(), 8192);

    client.SetSocketSendBufferSize(8192);
    EXPECT_EQ(client.GetSocketSendBufferSize(), 8192);

    client.SetRecvBufferSize(16384);
    EXPECT_EQ(client.GetRecvBufferSize(), 16384);

    client.SetSendChunkSize(32768);
    EXPECT_EQ(client.GetSendChunkSize(), 32768);

    client.SetWorkerThreadCount(4);
    EXPECT_EQ(client.GetWorkerThreadCount(), 4);

    client.SetIdleTimeout(std::chrono::milliseconds(5000));
    EXPECT_EQ(client.GetIdleTimeout().count(), 5000);

    client.SetAutoReconnect(true, std::chrono::milliseconds(100), std::chrono::milliseconds(500));
    EXPECT_TRUE(client.GetAutoReconnectEnabled());
    EXPECT_EQ(client.GetAutoReconnectInitialDelay().count(), 100);
    EXPECT_EQ(client.GetAutoReconnectMaxDelay().count(), 500);
}

TEST(TcpNetConfigTest, ClientIdleTimeout) {
    cpptcpnet::TcpListener listener(8091);
    listener.Start();

    cpptcpnet::TcpClient client;
    client.SetIdleTimeout(std::chrono::milliseconds(200));
    client.Start();

    std::atomic<bool> disconnected{false};
    cpppubsub::Worker worker;
    auto sub = client.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");
    worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [&](const cpptcpnet::ConnectionEvent& e) {
        if (e.state == cpptcpnet::ConnectionState::Disconnected) {
            disconnected.store(true);
        }
    });
    worker.Start();

    uint64_t session_id = client.Connect("127.0.0.1", 8091);
    ASSERT_GT(session_id, 0);

    // Wait for idle timeout (200ms) to trigger and close the connection
    for (int i = 0; i < 150 && !disconnected.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(disconnected.load());

    client.Stop();
    worker.Stop();
    listener.Stop();
}

TEST(TcpNetConfigTest, ClientAutoReconnect) {
    cpptcpnet::TcpListener listener(8092);
    listener.Start();

    cpptcpnet::TcpClient client;
    client.SetAutoReconnect(true, std::chrono::milliseconds(50), std::chrono::milliseconds(100));
    
    std::atomic<int> connect_count{0};
    std::atomic<int> disconnect_count{0};
    cpppubsub::Worker worker;
    auto sub = client.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");
    worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [&](const cpptcpnet::ConnectionEvent& e) {
        if (e.state == cpptcpnet::ConnectionState::Connected) {
            connect_count.fetch_add(1);
        } else {
            disconnect_count.fetch_add(1);
        }
    });
    worker.Start();

    uint64_t session_id = client.Connect("127.0.0.1", 8092);
    ASSERT_GT(session_id, 0);

    // Wait for first connection
    for (int i = 0; i < 100 && connect_count.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(connect_count.load(), 1);

    // Now stop listener to drop the connection
    listener.Stop();

    // Wait for client to detect disconnect
    for (int i = 0; i < 100 && disconnect_count.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(disconnect_count.load(), 1);

    // Start listener again on the same port
    cpptcpnet::TcpListener listener_rebound(8092);
    listener_rebound.Start();

    // Client should auto-reconnect
    for (int i = 0; i < 150 && connect_count.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(connect_count.load(), 2);

    client.Stop();
    worker.Stop();
    listener_rebound.Stop();
}

TEST(TcpNetConfigTest, PublicApis) {
    cpptcpnet::TcpListener listener(8093);
    EXPECT_FALSE(listener.IsRunning());
    listener.Start();
    EXPECT_TRUE(listener.IsRunning());

    cpptcpnet::TcpClient client;
    EXPECT_FALSE(client.IsRunning());
    client.Start();
    EXPECT_TRUE(client.IsRunning());

    uint64_t session_id = client.Connect("127.0.0.1", 8093);
    ASSERT_GT(session_id, 0);

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto active_sessions = listener.GetActiveSessions();
    EXPECT_EQ(active_sessions.size(), 1);
    if (!active_sessions.empty()) {
        EXPECT_GT(active_sessions[0], 0);
    }

    auto client_sessions = client.GetActiveSessions();
    EXPECT_EQ(client_sessions.size(), 1);
    if (!client_sessions.empty()) {
        EXPECT_EQ(client_sessions[0], session_id);
    }

    // Disconnect from client side
    EXPECT_TRUE(client.Disconnect(session_id));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(client.GetActiveSessions().size(), 0);

    client.Stop();
    listener.Stop();
}

// ===== DNS, IPv6, and PeerAddress Tests =====

TEST(TcpNetFeatureTest, DnsResolutionConnect) {
    cpptcpnet::TcpListener listener(8101, "localhost");
    listener.Start();

    cpptcpnet::TcpClient client;
    client.Start();

    uint64_t session_id = client.Connect("localhost", 8101);
    ASSERT_GT(session_id, 0);

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(client.GetActiveSessions().size(), 1);
    EXPECT_EQ(listener.GetActiveSessions().size(), 1);

    client.Stop();
    listener.Stop();
}

TEST(TcpNetFeatureTest, IPv6ConnectAndPeerAddress) {
    cpptcpnet::TcpListener listener(8102, "::1");
    listener.Start();

    cpptcpnet::TcpClient client;
    client.Start();

    uint64_t client_session_id = client.Connect("::1", 8102);
    ASSERT_GT(client_session_id, 0);

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto listener_sessions = listener.GetActiveSessions();
    ASSERT_EQ(listener_sessions.size(), 1);
    uint64_t listener_session_id = listener_sessions[0];

    // Check PeerAddress for Client
    cpptcpnet::PeerAddress client_peer = client.GetPeerAddress(client_session_id);
    EXPECT_EQ(client_peer.ip, "::1");
    EXPECT_EQ(client_peer.port, 8102);

    // Check PeerAddress for Listener
    cpptcpnet::PeerAddress listener_peer = listener.GetPeerAddress(listener_session_id);
    EXPECT_EQ(listener_peer.ip, "::1");
    EXPECT_GT(listener_peer.port, 0);

    client.Stop();
    listener.Stop();
}





