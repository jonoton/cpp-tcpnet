#include "cpptcpnet.hpp"
#include <iostream>
#include <string>

int main() {
    cpptcpnet::TcpListener server(8080);
    
    server.SetDataHandler([&server](uint64_t session_id, const std::vector<uint8_t>& data) {
        std::string text(data.begin(), data.end());
        std::cout << "Server received from session " << session_id << ": " << text << std::endl;
        server.Send(session_id, "Echo: " + text);
    });

    cpppubsub::Worker worker;
    auto sub = server.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>("state_events");
    worker.AddSubscription<cpptcpnet::ConnectionEvent>(sub, [](const cpptcpnet::ConnectionEvent& event) {
        if (event.state == cpptcpnet::ConnectionState::Connected) {
            std::cout << "Client connected: " << event.session_id << std::endl;
        } else {
            std::cout << "Client disconnected: " << event.session_id << std::endl;
        }
    });
    auto err_sub = server.GetEventBroker().Subscribe<cpptcpnet::ErrorEvent>("error_events");
    worker.AddSubscription<cpptcpnet::ErrorEvent>(err_sub, [](const cpptcpnet::ErrorEvent& event) {
        std::cerr << "Background error [" << event.error_code << "]: " << event.message << std::endl;
    });
    worker.Start();

    try {
        server.Start();
        std::cout << "Server running on port 8080. Press Enter to stop." << std::endl;
        std::cin.get();
        server.Stop();
        worker.Stop();
    } catch (const std::exception& e) {
        std::cerr << "Server startup failed: " << e.what() << std::endl;
    }

    return 0;
}
