#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "cpptcpnet.hpp"

int main() {
  cpptcpnet::TcpClient client;

  client.SetDataHandler(
      [](uint64_t session_id, const std::vector<uint8_t>& data) {
        std::string text(data.begin(), data.end());
        std::cout << "Client received: " << text << std::endl;
      });

  cpppubsub::Worker worker;
  auto sub = client.GetEventBroker().Subscribe<cpptcpnet::ConnectionEvent>(
      "state_events");
  worker.AddSubscription<cpptcpnet::ConnectionEvent>(
      sub, [](const cpptcpnet::ConnectionEvent& event) {
        if (event.state == cpptcpnet::ConnectionState::Connected) {
          std::cout << "Connected to server: " << event.session_id << std::endl;
        } else {
          std::cout << "Disconnected from server: " << event.session_id
                    << std::endl;
        }
      });
  auto err_sub =
      client.GetEventBroker().Subscribe<cpptcpnet::ErrorEvent>("error_events");
  worker.AddSubscription<cpptcpnet::ErrorEvent>(
      err_sub, [](const cpptcpnet::ErrorEvent& event) {
        std::cerr << "Background error [" << event.error_code
                  << "]: " << event.message << std::endl;
      });
  worker.Start();

  try {
    client.Connect("127.0.0.1", 8080);
    std::cout << "Sending data..." << std::endl;
    client.Send("Hello from client!");

    // Wait a bit to receive the echo
    std::this_thread::sleep_for(std::chrono::seconds(2));
    client.Stop();
  } catch (const std::exception& e) {
    std::cerr << "Failed to connect: " << e.what() << std::endl;
  }

  worker.Stop();
  return 0;
}
