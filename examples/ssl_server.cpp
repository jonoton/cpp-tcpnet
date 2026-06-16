#include <iostream>
#include <string>

#include "cpptcpnet.hpp"

#ifndef CPPTCPNET_CERTS_DIR
#define CPPTCPNET_CERTS_DIR "/workspaces/cpp-tcpnet/certs"
#endif

int main() {
  // Under testing guidelines, listen on localhost (127.0.0.1) instead of
  // 0.0.0.0
  cpptcpnet::TcpListener server(8443, "127.0.0.1");

#ifdef CPPTCPNET_SSL_SUPPORT
  cpptcpnet::TcpListener::SslConfig config;
  config.cert_file = CPPTCPNET_CERTS_DIR "/server.crt";
  config.key_file = CPPTCPNET_CERTS_DIR "/server.key";
  try {
    server.EnableSSL(config);
    std::cout << "SSL support enabled." << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "Failed to enable SSL: " << e.what() << std::endl;
    return 1;
  }
#else
  std::cout << "SSL support is NOT enabled in the library build." << std::endl;
#endif

  server.SetDataHandler(
      [&server](uint64_t session_id, const std::vector<uint8_t>& data) {
        std::string text(data.begin(), data.end());
        std::cout << "Server received from session " << session_id << ": "
                  << text << std::endl;
        server.Send(session_id, "Echo (SSL): " + text);
      });

  cpppubsub::Worker worker;
  auto sub = server.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>(
      "state_events");
  worker.AddSubscription<cpptcpnet::ConnectionEvent>(
      sub, [](const cpptcpnet::ConnectionEvent& event) {
        if (event.state == cpptcpnet::ConnectionState::Connected) {
          std::cout << "Client connected over SSL/TLS: " << event.session_id
                    << std::endl;
        } else {
          std::cout << "Client disconnected: " << event.session_id << std::endl;
        }
      });
  auto err_sub =
      server.GetEventBroker().Subscribe<cpptcpnet::ErrorEvent>("error_events");
  worker.AddSubscription<cpptcpnet::ErrorEvent>(
      err_sub, [](const cpptcpnet::ErrorEvent& event) {
        std::cerr << "Background error [" << event.error_code
                  << "]: " << event.message << std::endl;
      });
  worker.Start();

  try {
    server.Start();
    std::cout << "SSL Server running on 127.0.0.1:8443. Press Enter to stop."
              << std::endl;
    std::cin.get();
    server.Stop();
    worker.Stop();
  } catch (const std::exception& e) {
    std::cerr << "Server startup failed: " << e.what() << std::endl;
  }

  return 0;
}
