#include <iostream>
#include <string>
#include <any>
#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include "cpptcpnet.hpp"

using namespace cpptcpnet;

std::atomic<uint64_t> bytes_received(0);
std::atomic<uint64_t> client_bytes_received(0);
std::atomic<bool> client_connected(false);

#ifndef CPPTCPNET_CERTS_DIR
#define CPPTCPNET_CERTS_DIR "certs"
#endif

int PORT = 8443;

void RunServer() {
    TcpListener server(PORT, "127.0.0.1");

#ifdef CPPTCPNET_SSL_SUPPORT
    TcpListener::SslConfig config;
    config.cert_file = CPPTCPNET_CERTS_DIR "/server.crt";
    config.key_file = CPPTCPNET_CERTS_DIR "/server.key";

    // Verify cert files exist before starting SSL to prevent silent failures
    {
        std::ifstream cert_file(config.cert_file);
        std::ifstream key_file(config.key_file);
        if (!cert_file.good() || !key_file.good()) {
            std::cerr << "[Server Error] Certificate or private key file not found at " CPPTCPNET_CERTS_DIR << std::endl;
            return;
        }
    }

    try {
        server.EnableSSL(config);
        std::cout << "[Server] SSL enabled." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Server Error] Failed to enable SSL: " << e.what() << std::endl;
        return;
    }
#endif

    cpppubsub::Worker worker;
    auto sub = server.GetEventBroker().Subscribe<ConnectionEvent>("state_events");
    worker.AddSubscription<ConnectionEvent>(sub, [](const ConnectionEvent& e) {
        if (e.state == ConnectionState::Connected) {
            std::cout << "[Server] Client connected: " << e.session_id << std::endl;
        } else {
            std::cout << "[Server] Client disconnected: " << e.session_id << std::endl;
        }
    });
    worker.Start();

    server.SetDataHandler([&server](uint64_t session_id, const std::vector<uint8_t>& data) {
        bytes_received += data.size();
        server.Send(session_id, data);
    });

    server.SetErrorHandler([](int code, const std::string& msg) {
        std::cerr << "[Server Error] " << msg << " (Code: " << code << ")" << std::endl;
    });

    server.SetMaxOutboundBufferSize(100 * 1024 * 1024);
    
    try {
        server.Start();
        std::cout << "[Server] Started." << std::endl;
        auto server_start = std::chrono::steady_clock::now();
        while (bytes_received < 10 * 1024 * 1024) {
            if (std::chrono::steady_clock::now() - server_start > std::chrono::seconds(30)) {
                std::cerr << "[Server] Timed out waiting for data." << std::endl;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Wait for the client to disconnect after it has read all the echoed data
        while (server.GetStats().active_connections > 0) {
            if (std::chrono::steady_clock::now() - server_start > std::chrono::seconds(35)) {
                std::cerr << "[Server] Timed out waiting for client to disconnect." << std::endl;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        server.Stop();
        std::cout << "[Server] Stopped." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Server Error] Server failed: " << e.what() << std::endl;
    }

    worker.Stop();
    
}

void RunClient() {
    TcpClient client;

#ifdef CPPTCPNET_SSL_SUPPORT
    try {
        client.EnableSSL();
        std::cout << "[Client] SSL enabled." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Client Error] Failed to enable SSL: " << e.what() << std::endl;
        return;
    }
#endif

    cpppubsub::Worker worker;
    auto sub = client.GetEventBroker().Subscribe<ConnectionEvent>("state_events");
    worker.AddSubscription<ConnectionEvent>(sub, [](const ConnectionEvent& e) {
        if (e.state == ConnectionState::Connected) {
            std::cout << "[Client] Connected to server: " << e.session_id << std::endl;
            client_connected = true;
        } else {
            std::cout << "[Client] Disconnected from server: " << e.session_id << std::endl;
        }
    });
    worker.Start();
    std::cout << "[Client] Worker Started" << std::endl;

    client.SetDataHandler([](uint64_t, const std::vector<uint8_t>& data) {
        client_bytes_received += data.size();
    });

    client.SetMaxOutboundBufferSize(100 * 1024 * 1024);
    client.Start();
    std::cout << "[Client] Started" << std::endl;
    
    // Retry connection multiple times in case the server is still initializing
    bool connected = false;
    const int max_retries = 15;
    for (int retry = 0; retry < max_retries; ++retry) {
        try {
            client.Connect("127.0.0.1", PORT);
            connected = true;
            break;
        } catch (const std::system_error& e) {
            std::cout << "[Client] Connection attempt " << (retry + 1) << " failed: " << e.what() << ". Retrying..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    if (!connected) {
        std::cerr << "[Client Error] Failed to connect after multiple attempts." << std::endl;
        client.Stop();
        worker.Stop();
        return;
    }

    // Wait for connection and SSL handshake to actually establish before sending
    auto start_time = std::chrono::steady_clock::now();
    while (!client_connected) {
        if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(15)) {
            std::cerr << "[Client Error] Connection / SSL Handshake timeout!" << std::endl;
            client.Stop();
            worker.Stop();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[Client] Sending secure payload..." << std::endl;
    // Send a 10MB payload securely
    std::vector<uint8_t> large_payload(10 * 1024 * 1024, 'A'); // 10MB
    
    ThroughputTracker tracker(client.GetEventBroker());
    
    auto transfer_start = std::chrono::steady_clock::now();

    std::cout << "[Client] Sending large payload" << std::endl;
    client.Send(large_payload);

    std::cout << "[Client] Secure data sent to buffer. Waiting to flush and receive echo..." << std::endl;
    
    double peak_send = 0.0;
    double peak_recv = 0.0;
    
    auto wait_start = std::chrono::steady_clock::now();
    while (client_bytes_received < large_payload.size()) {
        if (std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(30)) {
            std::cerr << "[Client] Timed out waiting for echo!" << std::endl;
            break;
        }
        double current_send = tracker.GetSendThroughputBytesPerSec();
        double current_recv = tracker.GetRecvThroughputBytesPerSec();
        if (current_send > peak_send) peak_send = current_send;
        if (current_recv > peak_recv) peak_recv = current_recv;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    auto transfer_end = std::chrono::steady_clock::now();
    std::chrono::duration<double> duration = transfer_end - transfer_start;
    double elapsed = duration.count();

    client.Stop();
    worker.Stop();
    std::cout << "[Client] Stopped." << std::endl;
    
    double avg_send = elapsed > 0 ? (large_payload.size() / elapsed) : 0.0;
    double avg_recv = elapsed > 0 ? (client_bytes_received.load() / elapsed) : 0.0;
    
    auto format_bytes = [](uint64_t bytes) {
        auto scaled = ScaleBytes(bytes);
        char buf[128];
        snprintf(buf, sizeof(buf), "%.2f %s", scaled.value, scaled.unit);
        return std::string(buf);
    };

    auto format_tp = [](double bytes_per_sec) {
        auto scaled_bytes = ScaleBytes(static_cast<uint64_t>(bytes_per_sec));
        auto scaled_bits = ScaleBits(bytes_per_sec * 8.0);
        char buf[128];
        snprintf(buf, sizeof(buf), "%.2f %s/s (%.2f %s)", scaled_bytes.value, scaled_bytes.unit, scaled_bits.value, scaled_bits.unit);
        return std::string(buf);
    };
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "     SECURE CLIENT THROUGHPUT METRICS   " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Transfer Duration: " << elapsed << " seconds" << std::endl;
    std::cout << "Total Bytes Sent: " << large_payload.size() << " (" << format_bytes(large_payload.size()) << ")" << std::endl;
    std::cout << "Total Bytes Received: " << client_bytes_received.load() << " (" << format_bytes(client_bytes_received.load()) << ")" << std::endl;
    std::cout << "Average Send Throughput: " << format_tp(avg_send) << std::endl;
    std::cout << "Average Recv Throughput: " << format_tp(avg_recv) << std::endl;
    std::cout << "Peak Send Throughput (sliding): " << format_tp(peak_send) << std::endl;
    std::cout << "Peak Recv Throughput (sliding): " << format_tp(peak_recv) << std::endl;
    std::cout << "========================================\n" << std::endl;
}

int main() {
#ifndef CPPTCPNET_SSL_SUPPORT
    std::cout << "SSL/TLS support is NOT enabled in the library build. Skipping SSL test client/server example." << std::endl;
    return 0;
#endif

    SetLogger([](LogSeverity severity, const std::string& cls, const std::string& msg) {
        std::string severity_str = "INFO";
        if (severity == LogSeverity::Warning) severity_str = "WARN";
        else if (severity == LogSeverity::Error) severity_str = "ERROR";
        std::cout << "[" << severity_str << "][" << cls << "] " << msg << std::endl;
    });

    std::thread server_thread(RunServer);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    std::thread client_thread(RunClient);

    client_thread.join();
    server_thread.join();

    auto scaled = ScaleBytes(bytes_received.load());
    char buf[128];
    snprintf(buf, sizeof(buf), "%.2f %s", scaled.value, scaled.unit);
    std::cout << "Secure Test completed. Total bytes received by server: " << bytes_received.load() << " (" << buf << ")" << std::endl;
    return 0;
}
