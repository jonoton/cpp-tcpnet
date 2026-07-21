#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#ifdef CPPTCPNET_SSL_SUPPORT
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <mstcpip.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

using socket_t = SOCKET;
using pollfd_t = WSAPOLLFD;
#define POLL_FUNC WSAPoll
#define CLOSE_SOCKET closesocket
#define IN_PROGRESS_ERROR WSAEWOULDBLOCK
#define WOULD_BLOCK_ERROR WSAEWOULDBLOCK
#define GET_SOCKET_ERROR WSAGetLastError()
using ssize_t = int;
using socket_buf_size_t = int;
#define SEND_FLAGS 0
#define TIMEOUT_ERROR_CODE WSAETIMEDOUT
#define INTR_ERROR WSAEINTR
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using socket_t = int;
using socket_buf_size_t = size_t;
using pollfd_t = struct pollfd;
#define INVALID_SOCKET -1
#define POLL_FUNC poll
#define CLOSE_SOCKET close
#define IN_PROGRESS_ERROR EINPROGRESS
#define WOULD_BLOCK_ERROR EWOULDBLOCK
#define GET_SOCKET_ERROR errno
#ifdef __APPLE__
#define SEND_FLAGS 0
#else
#define SEND_FLAGS MSG_NOSIGNAL
#endif
#define TIMEOUT_ERROR_CODE ETIMEDOUT
#define INTR_ERROR EINTR
#endif

#include "cppasyncworker.hpp"
#include "cpppubsub.hpp"

namespace cpptcpnet {
constexpr int VERSION_MAJOR = 1;
constexpr int VERSION_MINOR = 4;
constexpr int VERSION_PATCH = 0;

/**
 * @brief Returns the library version as a string.
 * @return A reference to the version string in "MAJOR.MINOR.PATCH" format.
 */
inline const std::string &version() {
  static const std::string version_str = []() {
    return std::to_string(VERSION_MAJOR) + "." + std::to_string(VERSION_MINOR) +
           "." + std::to_string(VERSION_PATCH);
  }();
  return version_str;
}

/**
 * @brief Retrieves the last underlying OS socket error code.
 * @return The error code.
 */
inline int GetLastSocketError() { return GET_SOCKET_ERROR; }

/**
 * @brief A structure representing a scaled numeric value and its corresponding
 * unit label.
 */
struct ScaledUnit {
  double value;
  const char *unit;
};

/**
 * @brief Scales a byte count to its most appropriate binary unit (B, KB, MB,
 * GB, TB).
 * @param bytes The raw byte count.
 * @return A ScaledUnit structure containing the scaled value and unit label.
 */
inline ScaledUnit ScaleBytes(uint64_t bytes) {
  double size = static_cast<double>(bytes);
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int unit_idx = 0;
  while (size >= 1024.0 && unit_idx < 4) {
    size /= 1024.0;
    unit_idx++;
  }
  return {size, units[unit_idx]};
}

/**
 * @brief Scales a bit rate (bits per second) to its most appropriate decimal
 * unit (bps, Kbps, Mbps, Gbps, Tbps).
 * @param bits_per_sec The raw bit rate.
 * @return A ScaledUnit structure containing the scaled value and unit label.
 */
inline ScaledUnit ScaleBits(double bits_per_sec) {
  double rate = bits_per_sec;
  const char *units[] = {"bps", "Kbps", "Mbps", "Gbps", "Tbps"};
  int unit_idx = 0;
  while (rate >= 1000.0 && unit_idx < 4) {
    rate /= 1000.0;
    unit_idx++;
  }
  return {rate, units[unit_idx]};
}

/**
 * @brief Performs a high-precision, CPU-friendly yield sleep until a target
 * time point is reached. Helpful for sub-millisecond pacing delays.
 */
inline void PreciseSleepUntil(std::chrono::steady_clock::time_point target) {
  while (std::chrono::steady_clock::now() < target) {
    std::this_thread::yield();
  }
}

/**
 * @brief Represents a remote peer's address (IP and port).
 */
struct PeerAddress {
  std::string ip;
  uint16_t port = 0;
};

/**
 * @brief Represents the severity of a log message.
 */
enum class LogSeverity { Info, Warning, Error };

/**
 * @brief Callback type for custom logging.
 * @param severity The severity of the log message.
 * @param className The name of the class originating the log.
 * @param message The log message.
 */
using LogCallback =
    std::function<void(LogSeverity severity, const std::string &className,
                       const std::string &message)>;

namespace internal {
inline PeerAddress SockAddrToPeerAddress(const sockaddr *addr) {
  PeerAddress peer;
  if (addr->sa_family == AF_INET) {
    const auto *addr_in = reinterpret_cast<const sockaddr_in *>(addr);
    char ip_str[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &(addr_in->sin_addr), ip_str, sizeof(ip_str))) {
      peer.ip = ip_str;
    }
    peer.port = ntohs(addr_in->sin_port);
  } else if (addr->sa_family == AF_INET6) {
    const auto *addr_in6 = reinterpret_cast<const sockaddr_in6 *>(addr);
    char ip_str[INET6_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET6, &(addr_in6->sin6_addr), ip_str, sizeof(ip_str))) {
      peer.ip = ip_str;
    }
    peer.port = ntohs(addr_in6->sin6_port);
  }
  return peer;
}

inline PeerAddress GetPeerAddressForSocket(socket_t fd) {
  sockaddr_storage addr{};
  socklen_t addr_len = sizeof(addr);
  if (getpeername(fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) < 0) {
    int err = GetLastSocketError();
    throw std::system_error(err, std::system_category(), "getpeername failed");
  }
  return SockAddrToPeerAddress(reinterpret_cast<const sockaddr *>(&addr));
}
#ifdef CPPTCPNET_SSL_SUPPORT
inline void InitOpenSSL() {
  static std::once_flag init_flag;
  std::call_once(init_flag, []() {
    OPENSSL_init_ssl(
        OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
        nullptr);
  });
}
#endif

inline uint64_t GenerateRandomSessionId() {
  static std::mutex mtx;
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  std::lock_guard<std::mutex> lock(mtx);
  uint64_t id = 0;
  while (id == 0) {
    id = gen();
  }
  return id;
}

inline std::mutex &GetLoggerMutex() {
  static std::mutex mtx;
  return mtx;
}

inline LogCallback &GetLogger() {
  static LogCallback logger = nullptr;
  return logger;
}

class WakeUpChannel {
 public:
  WakeUpChannel() : read_fd_(INVALID_SOCKET), write_fd_(INVALID_SOCKET) {}
  ~WakeUpChannel() { Close(); }

  WakeUpChannel(const WakeUpChannel &) = delete;
  WakeUpChannel &operator=(const WakeUpChannel &) = delete;
  WakeUpChannel(WakeUpChannel &&) = delete;
  WakeUpChannel &operator=(WakeUpChannel &&) = delete;

  bool Initialize() {
#ifdef _WIN32
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      CLOSE_SOCKET(listener);
      return false;
    }

    if (listen(listener, 1) < 0) {
      CLOSE_SOCKET(listener);
      return false;
    }

    socklen_t addr_len = sizeof(addr);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &addr_len) <
        0) {
      CLOSE_SOCKET(listener);
      return false;
    }

    write_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (write_fd_ == INVALID_SOCKET) {
      CLOSE_SOCKET(listener);
      return false;
    }

    // Set write_fd_ to non-blocking before connect to prevent blocking
    // indefinitely
    u_long mode = 1;
    if (ioctlsocket(write_fd_, FIONBIO, &mode) < 0) {
      CLOSE_SOCKET(write_fd_);
      CLOSE_SOCKET(listener);
      return false;
    }

    // Set listener to non-blocking so accept doesn't block indefinitely
    if (ioctlsocket(listener, FIONBIO, &mode) < 0) {
      CLOSE_SOCKET(write_fd_);
      CLOSE_SOCKET(listener);
      return false;
    }

    int res =
        connect(write_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (res < 0) {
      int err = GetLastSocketError();
      if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
        CLOSE_SOCKET(write_fd_);
        CLOSE_SOCKET(listener);
        return false;
      }
    }

    // Get local address/port of write_fd_
    sockaddr_in local_write_addr{};
    socklen_t local_write_len = sizeof(local_write_addr);
    if (getsockname(write_fd_, reinterpret_cast<sockaddr *>(&local_write_addr),
                    &local_write_len) < 0) {
      CLOSE_SOCKET(write_fd_);
      CLOSE_SOCKET(listener);
      return false;
    }

    // Loop accepting connections until we get the one from our own write_fd_ or
    // timeout
    socket_t accepted_fd = INVALID_SOCKET;
    auto start_time = std::chrono::steady_clock::now();
    while (true) {
      if (std::chrono::steady_clock::now() - start_time >
          std::chrono::seconds(2)) {
        break;  // Timeout
      }

      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(listener, &read_fds);
      timeval tv{0, 100000};  // 100ms
      int select_res = select(0, &read_fds, nullptr, nullptr, &tv);
      if (select_res > 0) {
        sockaddr_in peer_addr{};
        socklen_t peer_len = sizeof(peer_addr);
        socket_t client = accept(
            listener, reinterpret_cast<sockaddr *>(&peer_addr), &peer_len);
        if (client != INVALID_SOCKET) {
          if (peer_addr.sin_port == local_write_addr.sin_port &&
              peer_addr.sin_addr.s_addr == local_write_addr.sin_addr.s_addr) {
            accepted_fd = client;
            break;
          } else {
            // Unauthorized connection, close immediately
            CLOSE_SOCKET(client);
          }
        }
      } else if (select_res < 0) {
        break;
      }
    }

    if (accepted_fd == INVALID_SOCKET) {
      CLOSE_SOCKET(write_fd_);
      CLOSE_SOCKET(listener);
      return false;
    }

    read_fd_ = accepted_fd;

    // Ensure read_fd_ is non-blocking (write_fd_ is already non-blocking)
    if (ioctlsocket(read_fd_, FIONBIO, &mode) < 0) {
      CLOSE_SOCKET(read_fd_);
      CLOSE_SOCKET(write_fd_);
      CLOSE_SOCKET(listener);
      return false;
    }

    CLOSE_SOCKET(listener);
    return true;
#else
    int fds[2];
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
      return false;
    }
#else
    if (pipe(fds) < 0) {
      return false;
    }
    int flags1 = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, flags1 | O_NONBLOCK);
    int flags2 = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, flags2 | O_NONBLOCK);
#endif
    read_fd_ = fds[0];
    write_fd_ = fds[1];
    return true;
#endif
  }

  void Trigger() {
    if (write_fd_ != INVALID_SOCKET) {
      char val = 1;
#ifdef _WIN32
      send(write_fd_, &val, 1, 0);
#else
      ssize_t res = write(write_fd_, &val, 1);
      (void)res;
#endif
    }
  }

  void Clear() {
    if (read_fd_ != INVALID_SOCKET) {
      char buf[128];
      ssize_t n;
      do {
#ifdef _WIN32
        n = recv(read_fd_, buf, sizeof(buf), 0);
#else
        n = read(read_fd_, buf, sizeof(buf));
#endif
      } while (n > 0);
    }
  }

  socket_t ReadFd() const { return read_fd_; }

  void Close() {
    if (read_fd_ != INVALID_SOCKET) {
      CLOSE_SOCKET(read_fd_);
      read_fd_ = INVALID_SOCKET;
    }
    if (write_fd_ != INVALID_SOCKET) {
      CLOSE_SOCKET(write_fd_);
      write_fd_ = INVALID_SOCKET;
    }
  }

 private:
  socket_t read_fd_;
  socket_t write_fd_;
};

using OutboundData = std::variant<std::vector<uint8_t>, std::string,
                                  std::shared_ptr<const std::vector<uint8_t>>,
                                  std::shared_ptr<const std::string>>;

struct OutboundChunk {
  OutboundData data;
  size_t offset = 0;

  const uint8_t *ptr() const {
    return std::visit(
        [this](const auto &val) -> const uint8_t * {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            return val.data() + offset;
          } else if constexpr (std::is_same_v<T, std::string>) {
            return reinterpret_cast<const uint8_t *>(val.data()) + offset;
          } else if constexpr (std::is_same_v<
                                   T, std::shared_ptr<
                                          const std::vector<uint8_t>>>) {
            return val ? (val->data() + offset) : nullptr;
          } else if constexpr (std::is_same_v<
                                   T, std::shared_ptr<const std::string>>) {
            return val ? (reinterpret_cast<const uint8_t *>(val->data()) +
                          offset)
                       : nullptr;
          }
          return nullptr;
        },
        data);
  }

  size_t size() const {
    return std::visit(
        [](const auto &val) -> size_t {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<
                            T, std::shared_ptr<const std::vector<uint8_t>>> ||
                        std::is_same_v<T, std::shared_ptr<const std::string>>) {
            return val ? val->size() : 0;
          } else {
            return val.size();
          }
        },
        data);
  }

  size_t remaining() const { return (offset < size()) ? (size() - offset) : 0; }
};

struct OutboundBuffer {
  std::deque<OutboundChunk> chunks;
  size_t total_bytes = 0;
};

template <typename T>
size_t GetDataSize(const T &val) {
  if constexpr (std::is_same_v<std::decay_t<T>,
                               std::shared_ptr<const std::vector<uint8_t>>> ||
                std::is_same_v<std::decay_t<T>,
                               std::shared_ptr<const std::string>>) {
    return val ? val->size() : 0;
  } else {
    return val.size();
  }
}

struct SocketOptions {
  bool no_delay = false;
  int socket_recv_buffer_size = 0;
  int socket_send_buffer_size = 0;
  bool keepalive_enabled = true;
  int keepalive_idle_secs = -1;
  int keepalive_interval_secs = -1;
  int keepalive_count = -1;
  bool linger_enabled = false;
  int linger_timeout_secs = 0;
};

inline void ApplySocketOptions(socket_t fd, const SocketOptions &opts) {
  if (opts.no_delay) {
    int opt = 1;
#ifdef _WIN32
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#endif
  }

  if (opts.socket_recv_buffer_size > 0) {
    int rcv_buf = opts.socket_recv_buffer_size;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char *>(&rcv_buf), sizeof(rcv_buf));
#else
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv_buf, sizeof(rcv_buf));
#endif
  }

  if (opts.socket_send_buffer_size > 0) {
    int snd_buf = opts.socket_send_buffer_size;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char *>(&snd_buf), sizeof(snd_buf));
#else
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_buf, sizeof(snd_buf));
#endif
  }

  if (opts.keepalive_enabled) {
    int opt = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#endif
    int idle = opts.keepalive_idle_secs;
    int interval = opts.keepalive_interval_secs;
    int count = opts.keepalive_count;

#ifdef _WIN32
    if (idle > 0 || interval > 0) {
      struct tcp_keepalive vals;
      vals.onoff = 1;
      vals.keepalivetime = (idle > 0) ? (idle * 1000) : 7200000;
      vals.keepaliveinterval = (interval > 0) ? (interval * 1000) : 1000;
      DWORD bytes_returned = 0;
      WSAIoctl(fd, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), nullptr, 0,
               &bytes_returned, nullptr, nullptr);
    }
#elif defined(__linux__)
    if (idle > 0) {
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    }
    if (interval > 0) {
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    }
    if (count > 0) {
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    }
#elif defined(__APPLE__)
    if (idle > 0) {
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
    }
#endif
  } else {
    int opt = 0;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#endif
  }

  if (opts.linger_enabled) {
    struct linger so_linger;
    so_linger.l_onoff = 1;
    so_linger.l_linger = opts.linger_timeout_secs;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_LINGER,
               reinterpret_cast<const char *>(&so_linger), sizeof(so_linger));
#else
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
#endif
  }
}
}  // namespace internal

/**
 * @brief Sets the global logger callback for the cpptcpnet library.
 * @param logger The logger callback function.
 */
inline void SetLogger(LogCallback logger) {
  std::lock_guard<std::mutex> lock(internal::GetLoggerMutex());
  internal::GetLogger() = std::move(logger);
}

/**
 * @brief Internal helper to log messages if a logger is set.
 */
inline void Log(LogSeverity severity, const std::string &className,
                const std::string &message) {
  LogCallback handler;
  {
    std::lock_guard<std::mutex> lock(internal::GetLoggerMutex());
    handler = internal::GetLogger();
  }
  if (handler) {
    handler(severity, className, message);
  }
}

/**
 * @brief Represents the current lifecycle state of a TCP connection.
 */
enum class ConnectionState { Connected, Disconnected };

/**
 * @brief A strongly-typed event broadcasted over the PubSub broker when a
 * socket changes state.
 */
struct ConnectionEvent {
  ConnectionState state;
  uint64_t session_id = 0;
};

/**
 * @brief A strongly-typed event broadcasted over the PubSub broker when a
 * background error occurs.
 */
struct ErrorEvent {
  int error_code;
  std::string message;
};

/**
 * @brief Event published over the PubSub broker when data is sent or received.
 */
struct TransferEvent {
  uint64_t session_id = 0;
  size_t bytes_transferred = 0;
  bool is_send = false;  // true if sent, false if received
};

/**
 * @brief TCP Keep-Alive configuration settings.
 */
struct KeepAliveConfig {
  bool enabled = true;
  int idle_secs = -1;      // -1 means OS default
  int interval_secs = -1;  // -1 means OS default
  int count = -1;          // -1 means OS default
};

/**
 * @brief TCP Linger configuration settings.
 */
struct LingerConfig {
  bool enabled = false;
  int timeout_secs = 0;
};

/**
 * @brief A configuration profile that groups socket and application-level
 * network parameters.
 */
struct ConnectionProfile {
  // Socket-level settings
  bool no_delay = false;
  int socket_recv_buffer_size = 0;  // 0 = OS default
  int socket_send_buffer_size = 0;  // 0 = OS default
  bool keepalive_enabled = true;
  int keepalive_idle_secs = -1;
  int keepalive_interval_secs = -1;
  int keepalive_count = -1;
  bool linger_enabled = false;
  int linger_timeout_secs = 0;

  // Application-level settings
  size_t recv_buffer_size = 4096;
  size_t send_chunk_size = 65536;
  size_t max_outbound_buffer_size = 10 * 1024 * 1024;

  // Timeouts
  std::chrono::milliseconds idle_timeout{60000};
  std::chrono::milliseconds send_timeout{30000};
  std::chrono::milliseconds connect_timeout{10000};

  /**
   * @brief Converts the profile settings into internal socket options.
   */
  internal::SocketOptions ToSocketOptions() const {
    internal::SocketOptions opts;
    opts.no_delay = no_delay;
    opts.socket_recv_buffer_size = socket_recv_buffer_size;
    opts.socket_send_buffer_size = socket_send_buffer_size;
    opts.keepalive_enabled = keepalive_enabled;
    opts.keepalive_idle_secs = keepalive_idle_secs;
    opts.keepalive_interval_secs = keepalive_interval_secs;
    opts.keepalive_count = keepalive_count;
    opts.linger_enabled = linger_enabled;
    opts.linger_timeout_secs = linger_timeout_secs;
    return opts;
  }

  /**
   * @brief Returns a preset profile optimized for high latency / high BDP
   * connections (e.g. satellite).
   */
  static ConnectionProfile HighLatency() {
    ConnectionProfile p;
    p.no_delay = true;
    p.socket_recv_buffer_size = 512 * 1024;  // 512KB for large TCP window
    p.socket_send_buffer_size = 512 * 1024;
    p.recv_buffer_size = 65536;  // 64KB application read buffer
    p.send_chunk_size = 65536;
    p.idle_timeout = std::chrono::milliseconds(120000);
    p.send_timeout = std::chrono::milliseconds(60000);
    p.connect_timeout = std::chrono::milliseconds(20000);
    return p;
  }

  /**
   * @brief Returns a preset profile optimized for low bandwidth / metered
   * connections (e.g. cellular 3G).
   */
  static ConnectionProfile LowBandwidth() {
    ConnectionProfile p;
    p.no_delay = false;  // Enable Nagle algorithm to merge small packets
    p.socket_recv_buffer_size = 16 * 1024;
    p.socket_send_buffer_size = 16 * 1024;
    p.recv_buffer_size = 1024;  // Small read buffer to prevent bufferbloat
    p.send_chunk_size = 1024;
    p.max_outbound_buffer_size = 512 * 1024;  // Limit queue memory
    p.idle_timeout = std::chrono::milliseconds(180000);
    p.send_timeout = std::chrono::milliseconds(120000);
    p.connect_timeout = std::chrono::milliseconds(30000);
    p.keepalive_idle_secs = 120;  // Heartbeat less frequently
    p.keepalive_interval_secs = 20;
    p.keepalive_count = 6;
    return p;
  }

  /**
   * @brief Returns a preset profile optimized for reliable, high-speed, low
   * latency local area networks.
   */
  static ConnectionProfile ReliableLAN() {
    ConnectionProfile p;
    p.no_delay = true;
    p.socket_recv_buffer_size = 64 * 1024;
    p.socket_send_buffer_size = 64 * 1024;
    p.recv_buffer_size = 8192;
    p.send_chunk_size = 16384;
    p.idle_timeout = std::chrono::milliseconds(15000);
    p.send_timeout = std::chrono::milliseconds(5000);
    p.connect_timeout = std::chrono::milliseconds(3000);
    return p;
  }
};

/**
 * @brief Accumulated statistics for a TcpListener.
 */
struct ListenerStats {
  uint64_t bytes_sent = 0;
  uint64_t bytes_received = 0;
  uint64_t packets_sent = 0;
  uint64_t packets_received = 0;
  uint64_t active_connections = 0;
  uint64_t total_connections = 0;
};

/**
 * @brief Accumulated statistics for a TcpClient.
 */
struct ClientStats {
  uint64_t bytes_sent = 0;
  uint64_t bytes_received = 0;
  uint64_t packets_sent = 0;
  uint64_t packets_received = 0;
  bool is_connected = false;
};

/**
 * @brief Callback type for handling asynchronous errors.
 * @param error_code The underlying OS error code.
 * @param message The description of the error.
 */
using ErrorHandler =
    std::function<void(int error_code, const std::string &message)>;

/**
 * @brief Helper function to set a socket to non-blocking mode.
 * @param fd The socket file descriptor to modify.
 */
inline bool SetNonBlocking(socket_t fd) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
#ifdef __APPLE__
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/**
 * @brief Manages a cross-platform, non-blocking TCP server event loop.
 * @warning This class transmits data in plaintext (cleartext) over raw TCP.
 *          It does NOT provide transport layer security (TLS/SSL). Do not use
 * this for transmitting sensitive or confidential information over public
 * networks without implementing an encryption layer on top of it.
 */
class TcpListener {
 public:
  /**
   * @brief Callback type for handling incoming data from a client.
   * @param session_id The unique session ID of the client sending data.
   * @param data The binary payload received.
   */
  using DataHandler = std::function<void(uint64_t session_id,
                                         const std::vector<uint8_t> &data)>;

  /**
   * @brief Constructs a new TcpListener.
   * @param port The port number to listen on.
   * @param bind_address The IP address to bind the listener to (defaults to
   * "0.0.0.0").
   * @param max_clients The maximum number of concurrent clients allowed.
   */
  TcpListener(uint16_t port, const std::string &bind_address = "0.0.0.0",
              size_t max_clients = 1024)
      : port_(port),
        bind_address_(bind_address),
        max_clients_(max_clients),
        running_(false),
        server_fd_(INVALID_SOCKET)
#ifdef CPPTCPNET_SSL_SUPPORT
        ,
        ssl_enabled_(false),
        ssl_ctx_(nullptr)
#endif
  {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
      wsa_initialized_ = true;
    } else {
      Log(LogSeverity::Error, "TcpListener", "WSAStartup failed.");
    }
#endif
    poll_fds_.reserve(max_clients_ + 1);
  }

  /**
   * @brief Destroys the TcpListener, stopping the server if running.
   */
  ~TcpListener() {
    Stop();
#ifdef _WIN32
    if (wsa_initialized_) {
      WSACleanup();
    }
#endif
  }

#ifdef CPPTCPNET_SSL_SUPPORT
  struct SslConfig {
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    bool require_client_cert = false;
    int verify_depth = -1;
    std::string cipher_list;
    std::string cipher_suites;
    int min_tls_version = -1;
    int max_tls_version = -1;
  };

  void EnableSSL(const SslConfig &config) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
      throw std::runtime_error("Cannot enable SSL while server is running.");
    }
    ssl_enabled_ = true;
    ssl_config_ = config;
  }
#endif

  void SetNoDelay(bool enabled) {
    no_delay_ = enabled;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.no_delay = enabled;
  }

  bool GetNoDelay() const { return no_delay_; }

  void SetSocketRecvBufferSize(int size) {
    socket_recv_buffer_size_ = size;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.socket_recv_buffer_size = size;
  }

  int GetSocketRecvBufferSize() const { return socket_recv_buffer_size_; }

  void SetSocketSendBufferSize(int size) {
    socket_send_buffer_size_ = size;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.socket_send_buffer_size = size;
  }

  int GetSocketSendBufferSize() const { return socket_send_buffer_size_; }

  void SetKeepAliveConfig(const KeepAliveConfig &config) {
    keepalive_enabled_ = config.enabled;
    keepalive_idle_secs_ = config.idle_secs;
    keepalive_interval_secs_ = config.interval_secs;
    keepalive_count_ = config.count;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.keepalive_enabled = config.enabled;
    default_profile_.keepalive_idle_secs = config.idle_secs;
    default_profile_.keepalive_interval_secs = config.interval_secs;
    default_profile_.keepalive_count = config.count;
  }

  KeepAliveConfig GetKeepAliveConfig() const {
    KeepAliveConfig config;
    config.enabled = keepalive_enabled_;
    config.idle_secs = keepalive_idle_secs_;
    config.interval_secs = keepalive_interval_secs_;
    config.count = keepalive_count_;
    return config;
  }

  void SetLinger(bool enabled, int timeout_secs) {
    linger_enabled_ = enabled;
    linger_timeout_secs_ = timeout_secs;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.linger_enabled = enabled;
    default_profile_.linger_timeout_secs = timeout_secs;
  }

  LingerConfig GetLinger() const {
    LingerConfig config;
    config.enabled = linger_enabled_;
    config.timeout_secs = linger_timeout_secs_;
    return config;
  }

  void SetReusePort(bool enabled) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
      throw std::runtime_error("Cannot set SO_REUSEPORT while running.");
    }
    reuse_port_ = enabled;
  }

  bool GetReusePort() const { return reuse_port_; }

  void SetListenBacklog(int backlog) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
      throw std::runtime_error("Cannot set listen backlog while running.");
    }
    listen_backlog_ = backlog;
  }

  int GetListenBacklog() const { return listen_backlog_; }

  /**
   * @brief Sets the number of background worker threads for handling callbacks.
   * @param threads The number of background threads.
   * @note Under the hood, this creates a vector of single-threaded worker
   * pools. Incoming data callbacks for any single session are pinned to the
   * same worker pool using a hash of the session ID. This guarantees session
   * affinity (serial, in-order execution of callbacks for any single session)
   *       while allowing concurrent processing across different sessions.
   * @note Must be called before Start(). Not thread-safe to call while the
   * server is running.
   */
  void SetWorkerThreadCount(size_t threads) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
      throw std::runtime_error("Cannot set worker thread count while running.");
    }
    worker_thread_count_ = threads;
  }

  size_t GetWorkerThreadCount() const { return worker_thread_count_; }

  void SetRecvBufferSize(size_t size) {
    recv_buffer_size_ = size;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.recv_buffer_size = size;
  }

  size_t GetRecvBufferSize() const { return recv_buffer_size_; }

  void SetSendChunkSize(size_t size) {
    send_chunk_size_ = size;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.send_chunk_size = size;
  }

  size_t GetSendChunkSize() const { return send_chunk_size_; }

  void SetSslHandshakeTimeout(std::chrono::milliseconds timeout) {
    ssl_handshake_timeout_ms_ = static_cast<uint32_t>(timeout.count());
  }

  std::chrono::milliseconds GetSslHandshakeTimeout() const {
    return std::chrono::milliseconds(
        ssl_handshake_timeout_ms_.load(std::memory_order_relaxed));
  }

  // Non-copyable and non-movable
  TcpListener(const TcpListener &) = delete;
  TcpListener &operator=(const TcpListener &) = delete;
  TcpListener(TcpListener &&) = delete;
  TcpListener &operator=(TcpListener &&) = delete;

  /**
   * @brief Sets the callback function invoked when data is received from a
   * client.
   * @param handler The data handler function.
   * @note Must be called before Start(). Not thread-safe to call while the
   * server is running.
   */
  void SetDataHandler(DataHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    data_handler_ = std::move(handler);
  }

  /**
   * @brief Sets the callback function invoked when a background error occurs.
   * @param handler The error handler function.
   * @note Must be called before Start(). Not thread-safe to call while the
   * server is running.
   */
  void SetErrorHandler(ErrorHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    error_handler_ = std::move(handler);
  }

  /**
   * @brief Sets the connection idle timeout.
   * @param timeout The duration after which an inactive connection is dropped.
   */
  void SetIdleTimeout(std::chrono::milliseconds timeout) {
    idle_timeout_ms_.store(static_cast<uint32_t>(timeout.count()),
                           std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.idle_timeout = timeout;
  }

  /**
   * @brief Gets the connection idle timeout.
   * @return The idle timeout duration.
   */
  std::chrono::milliseconds GetIdleTimeout() const {
    return std::chrono::milliseconds(
        idle_timeout_ms_.load(std::memory_order_relaxed));
  }

  /**
   * @brief Sets the connection send/write timeout.
   * @param timeout The duration after which a blocked write is timed out and
   * connection is dropped.
   */
  void SetSendTimeout(std::chrono::milliseconds timeout) {
    send_timeout_ms_.store(static_cast<uint32_t>(timeout.count()),
                           std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.send_timeout = timeout;
  }

  /**
   * @brief Gets the connection send/write timeout.
   * @return The send/write timeout duration.
   */
  std::chrono::milliseconds GetSendTimeout() const {
    return std::chrono::milliseconds(
        send_timeout_ms_.load(std::memory_order_relaxed));
  }

  /**
   * @brief Sets the listener file descriptor exhaustion throttling cooldown
   * duration.
   * @param cooldown The cooldown duration.
   */
  void SetThrottleCooldown(std::chrono::milliseconds cooldown) {
    throttle_cooldown_ms_.store(static_cast<uint32_t>(cooldown.count()),
                                std::memory_order_relaxed);
  }

  /**
   * @brief Gets the listener file descriptor exhaustion throttling cooldown
   * duration.
   * @return The cooldown duration.
   */
  std::chrono::milliseconds GetThrottleCooldown() const {
    return std::chrono::milliseconds(
        throttle_cooldown_ms_.load(std::memory_order_relaxed));
  }

  /**
   * @brief Sets the default connection profile for new connections.
   */
  void SetDefaultConnectionProfile(const ConnectionProfile &profile) {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_ = profile;
    // Synchronize individual atomic legacy variables
    no_delay_.store(profile.no_delay, std::memory_order_relaxed);
    socket_recv_buffer_size_.store(profile.socket_recv_buffer_size,
                                   std::memory_order_relaxed);
    socket_send_buffer_size_.store(profile.socket_send_buffer_size,
                                   std::memory_order_relaxed);
    keepalive_enabled_.store(profile.keepalive_enabled,
                             std::memory_order_relaxed);
    keepalive_idle_secs_.store(profile.keepalive_idle_secs,
                               std::memory_order_relaxed);
    keepalive_interval_secs_.store(profile.keepalive_interval_secs,
                                   std::memory_order_relaxed);
    keepalive_count_.store(profile.keepalive_count, std::memory_order_relaxed);
    linger_enabled_.store(profile.linger_enabled, std::memory_order_relaxed);
    linger_timeout_secs_.store(profile.linger_timeout_secs,
                               std::memory_order_relaxed);
    recv_buffer_size_.store(profile.recv_buffer_size,
                            std::memory_order_relaxed);
    send_chunk_size_.store(profile.send_chunk_size, std::memory_order_relaxed);
    max_outbound_buffer_size_.store(profile.max_outbound_buffer_size,
                                    std::memory_order_relaxed);
    idle_timeout_ms_.store(static_cast<uint32_t>(profile.idle_timeout.count()),
                           std::memory_order_relaxed);
    send_timeout_ms_.store(static_cast<uint32_t>(profile.send_timeout.count()),
                           std::memory_order_relaxed);
  }

  /**
   * @brief Gets the default connection profile.
   */
  ConnectionProfile GetDefaultConnectionProfile() const {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    return default_profile_;
  }

  /**
   * @brief Applies a connection profile to an active session dynamically
   * on-the-fly.
   */
  void ApplyConnectionProfile(uint64_t session_id,
                              const ConnectionProfile &profile) {
    socket_t fd = INVALID_SOCKET;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = session_to_fd_.find(session_id);
      if (it == session_to_fd_.end()) {
        throw std::runtime_error("Session not found: " +
                                 std::to_string(session_id));
      }
      fd = it->second;
      socket_profiles_[fd] = profile;
    }
    if (fd != INVALID_SOCKET) {
      internal::ApplySocketOptions(fd, profile.ToSocketOptions());
    }
  }

  /**
   * @brief Gets the active connection profile of a specific session.
   */
  ConnectionProfile GetConnectionProfile(uint64_t session_id) const {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    auto it = session_to_fd_.find(session_id);
    if (it == session_to_fd_.end()) {
      throw std::runtime_error("Session not found: " +
                               std::to_string(session_id));
    }
    auto prof_it = socket_profiles_.find(it->second);
    if (prof_it != socket_profiles_.end()) {
      return prof_it->second;
    }
    return default_profile_;
  }

  /**
   * @brief Retrieves the PubSub event broker for socket state changes.
   * @return A reference to the PubSub broker.
   */
  cpppubsub::PubSub &GetEventBroker() { return broker_; }

  /**
   * @brief Sets the maximum outbound buffer size per client in bytes.
   * @param max_size The maximum size of the outbound buffer.
   */
  void SetMaxOutboundBufferSize(size_t max_size) {
    max_outbound_buffer_size_.store(max_size, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.max_outbound_buffer_size = max_size;
  }

  /**
   * @brief Gets the maximum outbound buffer size per client in bytes.
   * @return The maximum size.
   */
  size_t GetMaxOutboundBufferSize() const {
    return max_outbound_buffer_size_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Starts the server listening for incoming connections on a background
   * thread.
   * @throws std::runtime_error If the server is already running or address is
   * invalid.
   * @throws std::system_error If socket creation, binding, or listening fails.
   */
  void Start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    if (running_) {
      Log(LogSeverity::Error, "TcpListener", "Server is already running.");
      throw std::runtime_error("[TcpListener] Server is already running.");
    }

#ifdef CPPTCPNET_SSL_SUPPORT
    if (ssl_enabled_) {
      internal::InitOpenSSL();
      ssl_ctx_ = SSL_CTX_new(TLS_server_method());
      if (!ssl_ctx_) {
        throw std::runtime_error("Failed to create SSL context.");
      }
      if (ssl_config_.min_tls_version >= 0) {
        SSL_CTX_set_min_proto_version(ssl_ctx_, ssl_config_.min_tls_version);
      } else {
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
      }
      if (ssl_config_.max_tls_version >= 0) {
        SSL_CTX_set_max_proto_version(ssl_ctx_, ssl_config_.max_tls_version);
      }
      if (!ssl_config_.cipher_list.empty()) {
        SSL_CTX_set_cipher_list(ssl_ctx_, ssl_config_.cipher_list.c_str());
      }
      if (!ssl_config_.cipher_suites.empty()) {
        SSL_CTX_set_ciphersuites(ssl_ctx_, ssl_config_.cipher_suites.c_str());
      }
      if (ssl_config_.require_client_cert) {
        int mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        SSL_CTX_set_verify(ssl_ctx_, mode, nullptr);
        if (ssl_config_.verify_depth >= 0) {
          SSL_CTX_set_verify_depth(ssl_ctx_, ssl_config_.verify_depth);
        }
      }
      if (!ssl_config_.ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(ssl_ctx_, ssl_config_.ca_file.c_str(),
                                          nullptr) <= 0) {
          SSL_CTX_free(ssl_ctx_);
          ssl_ctx_ = nullptr;
          throw std::runtime_error("Failed to load CA file: " +
                                   ssl_config_.ca_file);
        }
        STACK_OF(X509_NAME) *calist =
            SSL_load_client_CA_file(ssl_config_.ca_file.c_str());
        if (calist) {
          SSL_CTX_set_client_CA_list(ssl_ctx_, calist);
        }
      }
      if (SSL_CTX_use_certificate_chain_file(
              ssl_ctx_, ssl_config_.cert_file.c_str()) <= 0) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
        throw std::runtime_error("Failed to load certificate file: " +
                                 ssl_config_.cert_file);
      }
      if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, ssl_config_.key_file.c_str(),
                                      SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
        throw std::runtime_error("Failed to load private key file: " +
                                 ssl_config_.key_file);
      }
      if (SSL_CTX_check_private_key(ssl_ctx_) <= 0) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
        throw std::runtime_error("Private key does not match the certificate.");
      }
    }
#endif

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *res = nullptr;
    std::string port_str = std::to_string(port_);
    const char *node = bind_address_.empty() ? nullptr : bind_address_.c_str();

    int s = getaddrinfo(node, port_str.c_str(), &hints, &res);
    if (s != 0) {
      std::string err_msg =
          "getaddrinfo failed for bind address: " + bind_address_ + " error: ";
#ifdef _WIN32
      err_msg += std::to_string(WSAGetLastError());
#else
      err_msg += gai_strerror(s);
#endif
      Log(LogSeverity::Error, "TcpListener", err_msg);
      throw std::runtime_error(err_msg);
    }

    struct addrinfo *rp = nullptr;
    int last_err = 0;
    for (rp = res; rp != nullptr; rp = rp->ai_next) {
      server_fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (server_fd_ == INVALID_SOCKET) {
        last_err = GetLastSocketError();
        continue;
      }

      int opt = 1;
#ifdef _WIN32
      if (setsockopt(server_fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char *>(&opt), sizeof(opt)) < 0)
#else
      if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
          0)
#endif
      {
        last_err = GetLastSocketError();
        CLOSE_SOCKET(server_fd_);
        server_fd_ = INVALID_SOCKET;
        continue;
      }

#if defined(SO_REUSEPORT)
      if (reuse_port_.load(std::memory_order_relaxed)) {
        int reuse_opt = 1;
        if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT,
                       reinterpret_cast<const char *>(&reuse_opt),
                       sizeof(reuse_opt)) < 0) {
          last_err = GetLastSocketError();
          CLOSE_SOCKET(server_fd_);
          server_fd_ = INVALID_SOCKET;
          continue;
        }
      }
#endif

      if (!SetNonBlocking(server_fd_)) {
        last_err = GetLastSocketError();
        CLOSE_SOCKET(server_fd_);
        server_fd_ = INVALID_SOCKET;
        continue;
      }

      if (rp->ai_family == AF_INET6) {
        int no = 0;
        setsockopt(server_fd_, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char *>(&no), sizeof(no));
      }

      if (bind(server_fd_, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) ==
          0) {
        break;  // Success!
      }

      last_err = GetLastSocketError();
      CLOSE_SOCKET(server_fd_);
      server_fd_ = INVALID_SOCKET;
    }

    freeaddrinfo(res);

    if (server_fd_ == INVALID_SOCKET) {
      Log(LogSeverity::Error, "TcpListener",
          "Failed to bind to " + bind_address_ + ":" + std::to_string(port_));
      throw std::system_error(last_err, std::system_category(),
                              "Failed to bind");
    }

    int backlog = listen_backlog_.load(std::memory_order_relaxed);
    if (listen(server_fd_, backlog) < 0) {
      int err = GetLastSocketError();
      CLOSE_SOCKET(server_fd_);
      server_fd_ = INVALID_SOCKET;
      Log(LogSeverity::Error, "TcpListener", "Failed to listen on socket");
      throw std::system_error(err, std::system_category(),
                              "Failed to listen on socket");
    }

    if (!wakeup_channel_.Initialize()) {
      CLOSE_SOCKET(server_fd_);
      server_fd_ = INVALID_SOCKET;
      Log(LogSeverity::Error, "TcpListener",
          "Failed to initialize WakeUpChannel");
      throw std::runtime_error("Failed to initialize WakeUpChannel");
    }

    size_t threads = worker_thread_count_.load(std::memory_order_relaxed);
    if (threads == 0) {
      threads = std::thread::hardware_concurrency();
    }
    if (threads == 0) {
      threads = 1;
    }
    worker_pools_.resize(threads);
    for (size_t i = 0; i < threads; ++i) {
      worker_pools_[i] = std::make_unique<cppasyncworker::WorkerPool>(1);
    }

    running_ = true;
    try {
      loop_thread_ = std::thread(&TcpListener::RunPollLoop, this);
    } catch (...) {
      running_ = false;
      worker_pools_.clear();
      CLOSE_SOCKET(server_fd_);
      server_fd_ = INVALID_SOCKET;
      wakeup_channel_.Close();
      Log(LogSeverity::Error, "TcpListener",
          "Failed to start poll loop thread.");
      throw;
    }

    Log(LogSeverity::Info, "TcpListener",
        "Started on port " + std::to_string(port_));
  }

  /**
   * @brief Stops the server and closes all active connections.
   */
  void Stop(std::chrono::milliseconds drain_timeout) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!running_) return;

    if (server_fd_ != INVALID_SOCKET) {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      for (auto it = poll_fds_.begin(); it != poll_fds_.end(); ++it) {
        if (it->fd == server_fd_) {
          CLOSE_SOCKET(server_fd_);
          server_fd_ = INVALID_SOCKET;
          poll_fds_.erase(it);
          break;
        }
      }
    }

    auto start_time = std::chrono::steady_clock::now();
    while (drain_timeout.count() > 0) {
      bool all_drained = true;
      {
        std::lock_guard<std::mutex> out_lock(outbound_mutex_);
        for (const auto &pair : outbound_buffers_) {
          if (pair.second.total_bytes > 0) {
            all_drained = false;
            break;
          }
        }
      }
      if (all_drained) {
        break;
      }
      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (elapsed >= drain_timeout) {
        break;
      }
      wakeup_channel_.Trigger();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    StopInternalLocked();
  }

  void Stop() { Stop(std::chrono::milliseconds(0)); }

 private:
  void StopInternalLocked() {
    if (!running_) return;

    running_ = false;
    wakeup_channel_.Trigger();
    if (loop_thread_.joinable()) {
      if (loop_thread_.get_id() == std::this_thread::get_id()) {
        loop_thread_.detach();
      } else {
        loop_thread_.join();
      }
    }

    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
#ifdef CPPTCPNET_SSL_SUPPORT
      for (auto &pair : fd_to_ssl_) {
        SSL_shutdown(pair.second);
        SSL_free(pair.second);
      }
      fd_to_ssl_.clear();
      ssl_handshaking_fds_.clear();
      ssl_read_wants_read_.clear();
      ssl_read_wants_write_.clear();
      ssl_write_wants_read_.clear();
      ssl_write_wants_write_.clear();
      ssl_handshake_start_times_.clear();
#endif
      for (auto &pfd : poll_fds_) {
        if (pfd.fd != INVALID_SOCKET && pfd.fd != wakeup_channel_.ReadFd())
          CLOSE_SOCKET(pfd.fd);
      }
      poll_fds_.clear();
      if (server_fd_ != INVALID_SOCKET) {
        CLOSE_SOCKET(server_fd_);
        server_fd_ = INVALID_SOCKET;
      }
      fd_to_session_.clear();
      session_to_fd_.clear();
      last_activity_time_.clear();
      last_write_time_.clear();
    }
    {
      std::lock_guard<std::mutex> out_lock(outbound_mutex_);
      outbound_buffers_.clear();
    }
#ifdef CPPTCPNET_SSL_SUPPORT
    if (ssl_ctx_) {
      SSL_CTX_free(ssl_ctx_);
      ssl_ctx_ = nullptr;
    }
#endif
    wakeup_channel_.Close();
    worker_pools_.clear();
  }

 public:
  bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

  bool Disconnect(uint64_t session_id) {
    socket_t client_fd = INVALID_SOCKET;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = session_to_fd_.find(session_id);
      if (it != session_to_fd_.end()) {
        client_fd = it->second;
      }
    }
    if (client_fd != INVALID_SOCKET) {
      DisconnectClient(client_fd);
      return true;
    }
    return false;
  }

  std::vector<uint64_t> GetActiveSessions() const {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    std::vector<uint64_t> sessions;
    sessions.reserve(session_to_fd_.size());
    for (const auto &pair : session_to_fd_) {
      sessions.push_back(pair.first);
    }
    return sessions;
  }

  /**
   * @brief Retrieves the peer address (IP and port) of an active session.
   * @param session_id The session ID to query.
   * @return The PeerAddress struct.
   * @throws std::runtime_error If the session is not found or inactive.
   */
  PeerAddress GetPeerAddress(uint64_t session_id) const {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    auto it = session_to_fd_.find(session_id);
    if (it == session_to_fd_.end()) {
      throw std::runtime_error("Session not found: " +
                               std::to_string(session_id));
    }
    return internal::GetPeerAddressForSocket(it->second);
  }

  /**
   * @brief Sends binary data to a connected client using its session ID.
   * @param session_id The unique session ID of the client connection.
   * @param response The data payload to send.
   * @return True if successfully queued, false otherwise.
   */
  bool Send(uint64_t session_id, const std::vector<uint8_t> &response) {
    return SendImpl(session_id, response);
  }

  bool Send(uint64_t session_id, const std::string &response) {
    return SendImpl(session_id, response);
  }

  bool Send(uint64_t session_id, std::vector<uint8_t> &&response) {
    return SendImpl(session_id, std::move(response));
  }

  bool Send(uint64_t session_id, std::string &&response) {
    return SendImpl(session_id, std::move(response));
  }

  bool Send(uint64_t session_id,
            std::shared_ptr<const std::vector<uint8_t>> response) {
    return SendImpl(session_id, std::move(response));
  }

  bool Send(uint64_t session_id, std::shared_ptr<const std::string> response) {
    return SendImpl(session_id, std::move(response));
  }

  /**
   * @brief Retrieves a snapshot of the current listener statistics.
   * @return A ListenerStats struct.
   */
  ListenerStats GetStats() const {
    ListenerStats stats;
    stats.bytes_sent = bytes_sent_.load(std::memory_order_relaxed);
    stats.bytes_received = bytes_received_.load(std::memory_order_relaxed);
    stats.packets_sent = packets_sent_.load(std::memory_order_relaxed);
    stats.packets_received = packets_received_.load(std::memory_order_relaxed);
    stats.total_connections =
        total_connections_.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      stats.active_connections = fd_to_session_.size();
    }
    return stats;
  }

 private:
  uint16_t port_;
  std::string bind_address_;
  size_t max_clients_;
  std::atomic<bool> running_;
  socket_t server_fd_;
  std::thread loop_thread_;
  std::vector<pollfd_t> poll_fds_;
  // Lock ordering convention: Always acquire poll_mutex_ BEFORE outbound_mutex_
  // to prevent deadlocks.
  mutable std::mutex poll_mutex_;

  template <typename T>
  bool SendImpl(uint64_t session_id, T &&payload) {
    size_t payload_size = internal::GetDataSize(payload);
    if (payload_size == 0) {
      return true;
    }

    std::lock_guard<std::mutex> poll_lock(poll_mutex_);
    auto it = session_to_fd_.find(session_id);
    if (it == session_to_fd_.end()) {
      Log(LogSeverity::Warning, "TcpListener",
          "Send called on inactive or stale session_id: " +
              std::to_string(session_id));
      return false;
    }
    socket_t client_fd = it->second;
    auto prof_it = socket_profiles_.find(client_fd);
    ConnectionProfile prof = (prof_it != socket_profiles_.end())
                                 ? prof_it->second
                                 : default_profile_;

    {
      std::lock_guard<std::mutex> out_lock(outbound_mutex_);
      auto &out_buf = outbound_buffers_[client_fd];
      if (out_buf.total_bytes + payload_size > prof.max_outbound_buffer_size) {
        Log(LogSeverity::Error, "TcpListener",
            "Outbound buffer limit reached for socket fd: " +
                std::to_string(client_fd) + ". Dropping data.");
        return false;
      }
      out_buf.chunks.emplace_back(
          internal::OutboundChunk{std::forward<T>(payload), 0});
      out_buf.total_bytes += payload_size;
    }

    bool is_handshaking = false;
#ifdef CPPTCPNET_SSL_SUPPORT
    if (ssl_enabled_) {
      is_handshaking =
          ssl_handshaking_fds_.find(client_fd) != ssl_handshaking_fds_.end();
    }
#endif
    if (!is_handshaking) {
      for (auto &pfd : poll_fds_) {
        if (pfd.fd == client_fd) {
          pfd.events |= POLLOUT;
          break;
        }
      }
    }
    wakeup_channel_.Trigger();
    return true;
  }

  std::unordered_map<socket_t, internal::OutboundBuffer> outbound_buffers_;
  // Lock ordering convention: Always acquire poll_mutex_ BEFORE outbound_mutex_
  // to prevent deadlocks.
  std::mutex outbound_mutex_;
  std::atomic<size_t> max_outbound_buffer_size_ =
      10 * 1024 * 1024;  // 10MB default
  std::mutex lifecycle_mutex_;
#ifdef _WIN32
  bool wsa_initialized_ = false;
#endif

  mutable std::mutex handler_mutex_;
  DataHandler data_handler_;
  ErrorHandler error_handler_;
  cpppubsub::PubSub broker_;
  std::vector<std::unique_ptr<cppasyncworker::WorkerPool>> worker_pools_;
  std::atomic<size_t> worker_thread_count_{0};
  std::atomic<size_t> recv_buffer_size_{4096};
  std::atomic<size_t> send_chunk_size_{65536};
  std::atomic<uint32_t> ssl_handshake_timeout_ms_{10000};
  std::unordered_map<socket_t, std::chrono::steady_clock::time_point>
      ssl_handshake_start_times_;

  internal::WakeUpChannel wakeup_channel_;
  std::unordered_map<socket_t, uint64_t> fd_to_session_;
  std::unordered_map<uint64_t, socket_t> session_to_fd_;
  std::unordered_map<socket_t, std::chrono::steady_clock::time_point>
      last_activity_time_;
  std::unordered_map<socket_t, std::chrono::steady_clock::time_point>
      last_write_time_;
  std::atomic<uint32_t> idle_timeout_ms_{60000};     // 60 seconds default
  std::atomic<uint32_t> send_timeout_ms_{30000};     // 30 seconds default
  std::atomic<uint32_t> throttle_cooldown_ms_{200};  // 200ms default
  bool listener_throttled_ = false;
  std::chrono::steady_clock::time_point throttle_until_;
  std::atomic<uint64_t> bytes_sent_{0};
  std::atomic<uint64_t> bytes_received_{0};
  std::atomic<uint64_t> packets_sent_{0};
  std::atomic<uint64_t> packets_received_{0};
  std::atomic<uint64_t> total_connections_{0};

#ifdef CPPTCPNET_SSL_SUPPORT
  bool ssl_enabled_ = false;
  SslConfig ssl_config_;
  SSL_CTX *ssl_ctx_ = nullptr;
  std::unordered_map<socket_t, SSL *> fd_to_ssl_;
  std::unordered_set<socket_t> ssl_handshaking_fds_;
  // Per-operation SSL want states to avoid cross-contamination.
  // SSL_read returning WANT_READ is normal (wait for POLLIN to retry read).
  // SSL_read returning WANT_WRITE means renegotiation (wait for POLLOUT to
  // retry read). SSL_write returning WANT_READ means renegotiation (wait for
  // POLLIN to retry write). SSL_write returning WANT_WRITE is normal (wait for
  // POLLOUT to retry write).
  std::unordered_set<socket_t> ssl_read_wants_read_;
  std::unordered_set<socket_t> ssl_read_wants_write_;
  std::unordered_set<socket_t> ssl_write_wants_read_;
  std::unordered_set<socket_t> ssl_write_wants_write_;
#endif

  ConnectionProfile default_profile_;
  std::unordered_map<socket_t, ConnectionProfile> socket_profiles_;

  std::atomic<bool> no_delay_{false};
  std::atomic<int> socket_recv_buffer_size_{0};
  std::atomic<int> socket_send_buffer_size_{0};
  std::atomic<bool> keepalive_enabled_{true};
  std::atomic<int> keepalive_idle_secs_{-1};
  std::atomic<int> keepalive_interval_secs_{-1};
  std::atomic<int> keepalive_count_{-1};
  std::atomic<bool> linger_enabled_{false};
  std::atomic<int> linger_timeout_secs_{0};
  std::atomic<bool> reuse_port_{false};
  std::atomic<int> listen_backlog_{SOMAXCONN};

  void ApplySocketOptions(socket_t fd) {
    // Assumes poll_mutex_ is held
    auto it = socket_profiles_.find(fd);
    ConnectionProfile prof =
        (it != socket_profiles_.end()) ? it->second : default_profile_;
    internal::ApplySocketOptions(fd, prof.ToSocketOptions());
  }

  void ReportError(int error_code, const std::string &message) {
    Log(LogSeverity::Error, "TcpListener", message);
    ErrorHandler err_handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      err_handler = error_handler_;
    }
    if (err_handler && !worker_pools_.empty()) {
      (void)worker_pools_[0]->Enqueue([err_handler, error_code, message]() {
        try {
          err_handler(error_code, message);
        } catch (const std::exception &e) {
          Log(LogSeverity::Error, "TcpListener",
              "Exception in user error handler: " + std::string(e.what()));
        } catch (...) {
          Log(LogSeverity::Error, "TcpListener",
              "Unknown exception in user error handler");
        }
      });
    }
    broker_.Publish("error_events", ErrorEvent{error_code, message});
  }

  void RunPollLoop() {
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      poll_fds_.push_back({server_fd_, POLLIN, 0});
      poll_fds_.push_back({wakeup_channel_.ReadFd(), POLLIN, 0});
    }

    while (running_) {
      int timeout_ms = 1000;
      if (listener_throttled_) {
        auto now = std::chrono::steady_clock::now();
        if (now >= throttle_until_) {
          listener_throttled_ = false;
          std::lock_guard<std::mutex> lock(poll_mutex_);
          for (auto &pfd : poll_fds_) {
            if (pfd.fd == server_fd_) {
              pfd.events |= POLLIN;
              break;
            }
          }
        } else {
          auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  throttle_until_ - now)
                  .count();
          timeout_ms = (std::min)(1000, static_cast<int>(remaining));
          if (timeout_ms < 0) timeout_ms = 0;
        }
      }

#ifdef CPPTCPNET_SSL_SUPPORT
      // Check SSL handshake timeouts
      std::vector<socket_t> ssl_handshake_timeout_fds;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto ssl_timeout_duration = std::chrono::milliseconds(
            ssl_handshake_timeout_ms_.load(std::memory_order_relaxed));
        if (ssl_timeout_duration.count() > 0) {
          for (auto fd : ssl_handshaking_fds_) {
            auto it = ssl_handshake_start_times_.find(fd);
            if (it != ssl_handshake_start_times_.end() &&
                now - it->second > ssl_timeout_duration) {
              ssl_handshake_timeout_fds.push_back(fd);
            }
          }
        }
      }
      for (auto client_fd : ssl_handshake_timeout_fds) {
        Log(LogSeverity::Warning, "TcpListener",
            "Closing connection on fd " + std::to_string(client_fd) +
                " due to SSL handshake timeout.");
        DisconnectClient(client_fd);
      }
#endif

      // Check idle timeouts
      std::vector<socket_t> idle_connections;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (const auto &pair : last_activity_time_) {
          socket_t fd = pair.first;
          auto prof_it = socket_profiles_.find(fd);
          auto timeout_duration =
              (prof_it != socket_profiles_.end())
                  ? prof_it->second.idle_timeout
                  : std::chrono::milliseconds(
                        idle_timeout_ms_.load(std::memory_order_relaxed));
          if (timeout_duration.count() > 0 &&
              now - pair.second > timeout_duration) {
            idle_connections.push_back(fd);
          }
        }
      }
      for (auto client_fd : idle_connections) {
        Log(LogSeverity::Warning, "TcpListener",
            "Closing idle connection on fd " + std::to_string(client_fd));
        DisconnectClient(client_fd);
      }

      // Check send timeouts
      std::vector<socket_t> send_timed_out_connections;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        std::lock_guard<std::mutex> out_lock(outbound_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto &pair : last_write_time_) {
          socket_t fd = pair.first;
          auto it = outbound_buffers_.find(fd);
          bool has_pending =
              (it != outbound_buffers_.end() && it->second.total_bytes > 0);
          if (has_pending) {
            auto prof_it = socket_profiles_.find(fd);
            auto send_timeout_duration =
                (prof_it != socket_profiles_.end())
                    ? prof_it->second.send_timeout
                    : std::chrono::milliseconds(
                          send_timeout_ms_.load(std::memory_order_relaxed));
            if (send_timeout_duration.count() > 0 &&
                now - pair.second > send_timeout_duration) {
              send_timed_out_connections.push_back(fd);
            }
          } else {
            pair.second = now;
          }
        }
      }
      for (auto client_fd : send_timed_out_connections) {
        Log(LogSeverity::Warning, "TcpListener",
            "Closing connection on fd " + std::to_string(client_fd) +
                " due to send timeout.");
        DisconnectClient(client_fd);
      }

      std::vector<pollfd_t> poll_fds_copy;
      std::vector<uint64_t> session_ids;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        poll_fds_copy = poll_fds_;
        session_ids.reserve(poll_fds_.size());
        for (const auto &pfd : poll_fds_) {
          if (pfd.fd == server_fd_ || pfd.fd == wakeup_channel_.ReadFd()) {
            session_ids.push_back(0);
          } else {
            auto it = fd_to_session_.find(pfd.fd);
            session_ids.push_back(it != fd_to_session_.end() ? it->second : 0);
          }
        }
      }

      int poll_count;
      do {
#ifdef _WIN32
        poll_count =
            POLL_FUNC(poll_fds_copy.data(),
                      static_cast<ULONG>(poll_fds_copy.size()), timeout_ms);
#else
        poll_count =
            POLL_FUNC(poll_fds_copy.data(), poll_fds_copy.size(), timeout_ms);
#endif
      } while (poll_count < 0 && GetLastSocketError() == INTR_ERROR &&
               running_);

      if (poll_count < 0) {
        if (running_) {
          ReportError(GetLastSocketError(), "Poll error.");
        }
        break;
      }
      if (poll_count == 0) continue;

      for (size_t i = 0; i < poll_fds_copy.size(); ++i) {
        if (poll_fds_copy[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) {
          if (poll_fds_copy[i].fd == server_fd_) {
            if (poll_fds_copy[i].revents & POLLIN) AcceptConnection();
          } else if (poll_fds_copy[i].fd == wakeup_channel_.ReadFd()) {
            if (poll_fds_copy[i].revents & POLLIN) wakeup_channel_.Clear();
          } else {
            uint64_t expected_session_id = session_ids[i];
            if (expected_session_id != 0) {
              bool session_valid = false;
              {
                std::lock_guard<std::mutex> lock(poll_mutex_);
                auto it = fd_to_session_.find(poll_fds_copy[i].fd);
                if (it != fd_to_session_.end() &&
                    it->second == expected_session_id) {
                  session_valid = true;
                }
              }
              if (session_valid) {
                ProcessClientEvents(poll_fds_copy[i]);
              }
            }
          }
        }
      }
    }
  }

  void AcceptConnection() {
    while (running_) {
      sockaddr_storage client_addr{};
      socklen_t client_len = sizeof(client_addr);
      socket_t client_fd =
          accept(server_fd_, reinterpret_cast<struct sockaddr *>(&client_addr),
                 &client_len);

      if (client_fd == INVALID_SOCKET) {
        int err = GetLastSocketError();
        if (err == INTR_ERROR) {
          continue;
        }
        if (err == IN_PROGRESS_ERROR || err == WOULD_BLOCK_ERROR) {
          break;
        }
#ifdef _WIN32
        if (err == WSAEMFILE || err == WSAENOBUFS)
#else
        if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM)
#endif
        {
          listener_throttled_ = true;
          throttle_until_ =
              std::chrono::steady_clock::now() +
              std::chrono::milliseconds(
                  throttle_cooldown_ms_.load(std::memory_order_relaxed));
          std::lock_guard<std::mutex> lock(poll_mutex_);
          for (auto &pfd : poll_fds_) {
            if (pfd.fd == server_fd_) {
              pfd.events &= ~POLLIN;
              break;
            }
          }
          ReportError(
              err, "Accept failed due to resource limit, throttling listener.");
          break;
        }
        ReportError(err, "Accept failed.");
        break;
      }

      uint64_t session_id = 0;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        // poll_fds_ contains server_fd_ and wakeup_fd, so number of current
        // clients is poll_fds_.size() - 2
        if (poll_fds_.size() - 2 >= max_clients_) {
          CLOSE_SOCKET(client_fd);
          Log(LogSeverity::Warning, "TcpListener",
              "Max clients reached. Connection rejected.");
          continue;
        }

        if (!SetNonBlocking(client_fd)) {
          CLOSE_SOCKET(client_fd);
          Log(LogSeverity::Warning, "TcpListener",
              "Failed to set client non-blocking. Connection rejected.");
          continue;
        }

        socket_profiles_[client_fd] = default_profile_;
        ApplySocketOptions(client_fd);

        do {
          session_id = internal::GenerateRandomSessionId();
        } while (session_to_fd_.find(session_id) != session_to_fd_.end());

        fd_to_session_[client_fd] = session_id;
        session_to_fd_[session_id] = client_fd;
        last_activity_time_[client_fd] = std::chrono::steady_clock::now();
        last_write_time_[client_fd] = std::chrono::steady_clock::now();
#ifdef CPPTCPNET_SSL_SUPPORT
        if (ssl_enabled_) {
          SSL *ssl = SSL_new(ssl_ctx_);
          if (!ssl) {
            fd_to_session_.erase(client_fd);
            session_to_fd_.erase(session_id);
            last_activity_time_.erase(client_fd);
            last_write_time_.erase(client_fd);
            CLOSE_SOCKET(client_fd);
            Log(LogSeverity::Warning, "TcpListener",
                "Failed to create SSL object.");
            continue;
          }
          SSL_set_fd(ssl, static_cast<int>(client_fd));
          SSL_set_accept_state(ssl);
          fd_to_ssl_[client_fd] = ssl;
          ssl_handshaking_fds_.insert(client_fd);
          ssl_handshake_start_times_[client_fd] =
              std::chrono::steady_clock::now();
          poll_fds_.push_back({client_fd, POLLIN | POLLOUT, 0});
        } else {
          poll_fds_.push_back({client_fd, POLLIN, 0});
        }
#else
        poll_fds_.push_back({client_fd, POLLIN, 0});
#endif
        total_connections_.fetch_add(1, std::memory_order_relaxed);
      }

#ifdef CPPTCPNET_SSL_SUPPORT
      if (!ssl_enabled_ && session_id != 0) {
        broker_.Publish(
            "state_events",
            ConnectionEvent{ConnectionState::Connected, session_id});
      }
#else
      if (session_id != 0) {
        broker_.Publish(
            "state_events",
            ConnectionEvent{ConnectionState::Connected, session_id});
      }
#endif
    }
  }

#ifdef CPPTCPNET_SSL_SUPPORT
  void HandleSslHandshake(socket_t client_fd, uint64_t session_id,
                          bool &disconnect) {
    SSL *ssl = nullptr;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = fd_to_ssl_.find(client_fd);
      if (it == fd_to_ssl_.end()) return;
      ssl = it->second;
    }

    int ret = SSL_accept(ssl);
    if (ret <= 0) {
      int err = SSL_get_error(ssl, ret);
      if (err == SSL_ERROR_WANT_READ) {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        ssl_read_wants_read_.insert(client_fd);
        ssl_read_wants_write_.erase(client_fd);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == client_fd) {
            master_pfd.events = POLLIN;
            break;
          }
        }
      } else if (err == SSL_ERROR_WANT_WRITE) {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        ssl_read_wants_write_.insert(client_fd);
        ssl_read_wants_read_.erase(client_fd);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == client_fd) {
            master_pfd.events = POLLOUT;
            break;
          }
        }
      } else {
        disconnect = true;
        unsigned long oscode = ERR_get_error();
        char errbuf[256];
        ERR_error_string_n(oscode, errbuf, sizeof(errbuf));
        Log(LogSeverity::Warning, "TcpListener",
            "SSL handshake failed for fd " + std::to_string(client_fd) + ": " +
                errbuf);
      }
    } else {
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        ssl_handshaking_fds_.erase(client_fd);
        ssl_handshake_start_times_.erase(client_fd);
        ssl_read_wants_read_.erase(client_fd);
        ssl_read_wants_write_.erase(client_fd);
        ssl_write_wants_read_.erase(client_fd);
        ssl_write_wants_write_.erase(client_fd);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == client_fd) {
            master_pfd.events = POLLIN;
            break;
          }
        }
      }
      Log(LogSeverity::Info, "TcpListener",
          "SSL Handshake completed for session: " + std::to_string(session_id));
      broker_.Publish("state_events",
                      ConnectionEvent{ConnectionState::Connected, session_id});

      bool needs_pollout = false;
      {
        std::lock_guard<std::mutex> out_lock(outbound_mutex_);
        auto it = outbound_buffers_.find(client_fd);
        if (it != outbound_buffers_.end() && it->second.total_bytes > 0) {
          needs_pollout = true;
        }
      }
      if (needs_pollout) {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == client_fd) {
            master_pfd.events |= POLLOUT;
            break;
          }
        }
      }
    }
  }
#endif

  void ProcessClientEvents(const pollfd_t &pfd) {
    socket_t client_fd = pfd.fd;
    bool disconnect = false;

    uint64_t session_id = 0;
    ConnectionProfile prof;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = fd_to_session_.find(client_fd);
      if (it != fd_to_session_.end()) {
        session_id = it->second;
      }
      auto prof_it = socket_profiles_.find(client_fd);
      prof = (prof_it != socket_profiles_.end()) ? prof_it->second
                                                 : default_profile_;
    }

#ifdef CPPTCPNET_SSL_SUPPORT
    bool is_handshaking = false;
    SSL *ssl = nullptr;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      is_handshaking =
          ssl_handshaking_fds_.find(client_fd) != ssl_handshaking_fds_.end();
      auto it = fd_to_ssl_.find(client_fd);
      if (it != fd_to_ssl_.end()) ssl = it->second;
    }

    if (ssl_enabled_ && is_handshaking) {
      HandleSslHandshake(client_fd, session_id, disconnect);
      if (disconnect) {
        DisconnectClient(client_fd);
      }
      return;
    }

    if (ssl_enabled_ && ssl) {
      bool should_read = false;
      bool should_write = false;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        // SSL_read needs POLLOUT only if it previously returned WANT_WRITE
        // (renegotiation)
        bool read_wants_write = ssl_read_wants_write_.find(client_fd) !=
                                ssl_read_wants_write_.end();
        // SSL_write needs POLLIN only if it previously returned WANT_READ
        // (renegotiation)
        bool write_wants_read = ssl_write_wants_read_.find(client_fd) !=
                                ssl_write_wants_read_.end();

        // Read when: POLLIN fires (normal) OR POLLOUT fires and SSL_read needs
        // it (renegotiation)
        if ((pfd.revents & POLLIN) && !read_wants_write) {
          should_read = true;
        }
        if ((pfd.revents & POLLOUT) && read_wants_write) {
          should_read = true;
        }
        // Write when: POLLOUT fires (normal) OR POLLIN fires and SSL_write
        // needs it (renegotiation)
        if ((pfd.revents & POLLOUT) && !write_wants_read) {
          should_write = true;
        }
        if ((pfd.revents & POLLIN) && write_wants_read) {
          should_write = true;
        }
      }

      if (should_read) {
        bool read_more = true;
        while (read_more && !disconnect && running_) {
          size_t buf_size = prof.recv_buffer_size;
          std::vector<char> dynamic_buf;
          char stack_buf[4096];
          char *buffer = stack_buf;
          if (buf_size > 4096) {
            dynamic_buf.resize(buf_size);
            buffer = dynamic_buf.data();
          }
          int bytes_read = 0;
          int err = 0;
          while (running_) {
            bytes_read = SSL_read(ssl, buffer, static_cast<int>(buf_size));
            if (bytes_read >= 0) break;
            err = SSL_get_error(ssl, bytes_read);
            if (err == SSL_ERROR_SYSCALL && GET_SOCKET_ERROR == INTR_ERROR)
              continue;
            break;
          }

          if (bytes_read > 0) {
            {
              std::lock_guard<std::mutex> lock(poll_mutex_);
              ssl_read_wants_read_.erase(client_fd);
              ssl_read_wants_write_.erase(client_fd);
            }
            bytes_received_.fetch_add(bytes_read, std::memory_order_relaxed);
            packets_received_.fetch_add(1, std::memory_order_relaxed);
            broker_.Publish(
                "transfer_events",
                TransferEvent{session_id, static_cast<size_t>(bytes_read),
                              false});

            {
              std::lock_guard<std::mutex> lock(poll_mutex_);
              last_activity_time_[client_fd] = std::chrono::steady_clock::now();
            }

            std::vector<uint8_t> payload(buffer, buffer + bytes_read);

            DataHandler handler;
            {
              std::lock_guard<std::mutex> lock(handler_mutex_);
              handler = data_handler_;
            }
            if (handler && !worker_pools_.empty()) {
              size_t pool_idx = session_id % worker_pools_.size();
              (void)worker_pools_[pool_idx]->Enqueue(
                  [session_id, payload = std::move(payload),
                   handler = std::move(handler)]() {
                    try {
                      handler(session_id, payload);
                    } catch (const std::exception &e) {
                      Log(LogSeverity::Error, "TcpListener",
                          "Exception in user data handler: " +
                              std::string(e.what()));
                    } catch (...) {
                      Log(LogSeverity::Error, "TcpListener",
                          "Unknown exception in user data handler");
                    }
                  });
            }
          } else if (bytes_read == 0) {
            disconnect = true;
            read_more = false;
          } else {
            read_more = false;
            if (err == SSL_ERROR_WANT_READ) {
              std::lock_guard<std::mutex> lock(poll_mutex_);
              ssl_read_wants_read_.insert(client_fd);
              ssl_read_wants_write_.erase(client_fd);
            } else if (err == SSL_ERROR_WANT_WRITE) {
              std::lock_guard<std::mutex> lock(poll_mutex_);
              ssl_read_wants_write_.insert(client_fd);
              ssl_read_wants_read_.erase(client_fd);
            } else {
              disconnect = true;
            }
          }
        }
      }

      size_t sent_bytes_to_publish = 0;
      if (!disconnect && should_write) {
        std::lock_guard<std::mutex> poll_lock(poll_mutex_);
        std::lock_guard<std::mutex> out_lock(outbound_mutex_);
        auto it = outbound_buffers_.find(client_fd);
        if (it != outbound_buffers_.end()) {
          auto &out_buf = it->second;
          if (out_buf.total_bytes > 0) {
            auto &chunk = out_buf.chunks.front();
            size_t remaining = chunk.remaining();
            size_t to_send = (std::min)(remaining, prof.send_chunk_size);
            int sent = 0;
            int err = 0;
            while (running_) {
              sent = SSL_write(ssl, chunk.ptr(), static_cast<int>(to_send));
              if (sent > 0) break;
              err = SSL_get_error(ssl, sent);
              if (err == SSL_ERROR_SYSCALL && GET_SOCKET_ERROR == INTR_ERROR)
                continue;
              break;
            }

            if (sent > 0) {
              ssl_write_wants_read_.erase(client_fd);
              ssl_write_wants_write_.erase(client_fd);
              bytes_sent_.fetch_add(sent, std::memory_order_relaxed);
              packets_sent_.fetch_add(1, std::memory_order_relaxed);
              sent_bytes_to_publish = static_cast<size_t>(sent);
              last_activity_time_[client_fd] = std::chrono::steady_clock::now();
              last_write_time_[client_fd] = std::chrono::steady_clock::now();
              chunk.offset += sent;
              out_buf.total_bytes -= sent;
              if (chunk.offset == chunk.size()) {
                out_buf.chunks.pop_front();
              }
            } else {
              if (err == SSL_ERROR_WANT_READ) {
                ssl_write_wants_read_.insert(client_fd);
                ssl_write_wants_write_.erase(client_fd);
              } else if (err == SSL_ERROR_WANT_WRITE) {
                ssl_write_wants_write_.insert(client_fd);
                ssl_write_wants_read_.erase(client_fd);
              } else {
                disconnect = true;
              }
            }
          }

          if (out_buf.total_bytes == 0 &&
              ssl_write_wants_write_.find(client_fd) ==
                  ssl_write_wants_write_.end()) {
            last_write_time_[client_fd] = std::chrono::steady_clock::now();
          }
        }
      }
      if (sent_bytes_to_publish > 0) {
        broker_.Publish("transfer_events",
                        TransferEvent{session_id, sent_bytes_to_publish, true});
      }

      if (!disconnect) {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == client_fd) {
            short events = 0;
            bool has_outbound = false;
            {
              std::lock_guard<std::mutex> out_lock(outbound_mutex_);
              auto it = outbound_buffers_.find(client_fd);
              if (it != outbound_buffers_.end() && it->second.total_bytes > 0) {
                has_outbound = true;
              }
            }

            // Need POLLIN if: SSL_read wants it (normal), or SSL_write needs
            // POLLIN (renegotiation), or no special SSL state (default: always
            // listen for incoming data)
            bool need_pollin = ssl_read_wants_read_.find(client_fd) !=
                                   ssl_read_wants_read_.end() ||
                               ssl_write_wants_read_.find(client_fd) !=
                                   ssl_write_wants_read_.end() ||
                               (ssl_read_wants_write_.find(client_fd) ==
                                ssl_read_wants_write_.end());
            if (need_pollin) {
              events |= POLLIN;
            }
            // Need POLLOUT if: have data to send, or SSL_read needs POLLOUT
            // (renegotiation), or SSL_write needs POLLOUT (normal backpressure)
            if (has_outbound ||
                ssl_read_wants_write_.find(client_fd) !=
                    ssl_read_wants_write_.end() ||
                ssl_write_wants_write_.find(client_fd) !=
                    ssl_write_wants_write_.end()) {
              events |= POLLOUT;
            }
            master_pfd.events = events;
            break;
          }
        }
      }

      if (disconnect) {
        DisconnectClient(client_fd);
      }
      return;
    }
#endif

    if (pfd.revents & POLLIN) {
      size_t buf_size = prof.recv_buffer_size;
      std::vector<char> dynamic_buf;
      char stack_buf[4096];
      char *buffer = stack_buf;
      if (buf_size > 4096) {
        dynamic_buf.resize(buf_size);
        buffer = dynamic_buf.data();
      }
      ssize_t bytes_read;
      do {
        bytes_read = recv(client_fd, buffer,
                          static_cast<socket_buf_size_t>(buf_size), 0);
      } while (bytes_read < 0 && GetLastSocketError() == INTR_ERROR &&
               running_);

      if (bytes_read > 0) {
        bytes_received_.fetch_add(bytes_read, std::memory_order_relaxed);
        packets_received_.fetch_add(1, std::memory_order_relaxed);
        broker_.Publish(
            "transfer_events",
            TransferEvent{session_id, static_cast<size_t>(bytes_read), false});

        {
          std::lock_guard<std::mutex> lock(poll_mutex_);
          last_activity_time_[client_fd] = std::chrono::steady_clock::now();
        }

        std::vector<uint8_t> payload(buffer, buffer + bytes_read);

        DataHandler handler;
        {
          std::lock_guard<std::mutex> lock(handler_mutex_);
          handler = data_handler_;
        }
        if (handler && !worker_pools_.empty()) {
          size_t pool_idx = session_id % worker_pools_.size();
          (void)worker_pools_[pool_idx]->Enqueue([session_id,
                                                  payload = std::move(payload),
                                                  handler =
                                                      std::move(handler)]() {
            try {
              handler(session_id, payload);
            } catch (const std::exception &e) {
              Log(LogSeverity::Error, "TcpListener",
                  "Exception in user data handler: " + std::string(e.what()));
            } catch (...) {
              Log(LogSeverity::Error, "TcpListener",
                  "Unknown exception in user data handler");
            }
          });
        }
      } else if (bytes_read == 0) {
        disconnect = true;
      } else if (bytes_read < 0) {
        if (int err = GetLastSocketError();
            err != IN_PROGRESS_ERROR && err != WOULD_BLOCK_ERROR) {
          disconnect = true;
        }
      }
    }

    size_t sent_bytes_to_publish = 0;
    if (!disconnect && (pfd.revents & POLLOUT)) {
      std::lock_guard<std::mutex> poll_lock(poll_mutex_);
      std::lock_guard<std::mutex> out_lock(outbound_mutex_);
      auto it = outbound_buffers_.find(client_fd);
      if (it != outbound_buffers_.end()) {
        auto &out_buf = it->second;
        if (out_buf.total_bytes > 0) {
          auto &chunk = out_buf.chunks.front();
          size_t remaining = chunk.remaining();
          size_t to_send = (std::min)(remaining, prof.send_chunk_size);
          ssize_t sent;
          do {
            sent = send(client_fd, reinterpret_cast<const char *>(chunk.ptr()),
                        static_cast<socket_buf_size_t>(to_send), SEND_FLAGS);
          } while (sent < 0 && GetLastSocketError() == INTR_ERROR && running_);

          if (sent > 0) {
            bytes_sent_.fetch_add(sent, std::memory_order_relaxed);
            packets_sent_.fetch_add(1, std::memory_order_relaxed);
            sent_bytes_to_publish = static_cast<size_t>(sent);
            last_activity_time_[client_fd] = std::chrono::steady_clock::now();
            last_write_time_[client_fd] = std::chrono::steady_clock::now();
            chunk.offset += sent;
            out_buf.total_bytes -= sent;
            if (chunk.offset == chunk.size()) {
              out_buf.chunks.pop_front();
            }
          } else if (sent < 0) {
            if (int err = GetLastSocketError();
                err != IN_PROGRESS_ERROR && err != WOULD_BLOCK_ERROR) {
              disconnect = true;
            }
          }
        }

        if (out_buf.total_bytes == 0) {
          last_write_time_[client_fd] = std::chrono::steady_clock::now();
          for (auto &master_pfd : poll_fds_) {
            if (master_pfd.fd == client_fd) {
              master_pfd.events &= ~POLLOUT;
              break;
            }
          }
        }
      }
    }
    if (sent_bytes_to_publish > 0) {
      broker_.Publish("transfer_events",
                      TransferEvent{session_id, sent_bytes_to_publish, true});
    }

    if (!disconnect && (pfd.revents & (POLLERR | POLLHUP))) {
      disconnect = true;
    }

    if (disconnect) {
      DisconnectClient(client_fd);
    }
  }

  void DisconnectClient(socket_t client_fd) {
#ifdef CPPTCPNET_SSL_SUPPORT
    SSL *ssl = nullptr;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = fd_to_ssl_.find(client_fd);
      if (it != fd_to_ssl_.end()) {
        ssl = it->second;
        fd_to_ssl_.erase(it);
      }
      ssl_handshaking_fds_.erase(client_fd);
      ssl_handshake_start_times_.erase(client_fd);
      ssl_read_wants_read_.erase(client_fd);
      ssl_read_wants_write_.erase(client_fd);
      ssl_write_wants_read_.erase(client_fd);
      ssl_write_wants_write_.erase(client_fd);
    }
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
    }
#endif
    uint64_t session_id = 0;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it_sess = fd_to_session_.find(client_fd);
      if (it_sess != fd_to_session_.end()) {
        session_id = it_sess->second;
        fd_to_session_.erase(it_sess);
        session_to_fd_.erase(session_id);
      }
      socket_profiles_.erase(client_fd);
      last_activity_time_.erase(client_fd);
      last_write_time_.erase(client_fd);
      for (auto it = poll_fds_.begin(); it != poll_fds_.end(); ++it) {
        if (it->fd == client_fd) {
          poll_fds_.erase(it);
          break;
        }
      }
    }
    {
      std::lock_guard<std::mutex> out_lock(outbound_mutex_);
      outbound_buffers_.erase(client_fd);
    }
    CLOSE_SOCKET(client_fd);
    if (session_id != 0) {
      broker_.Publish(
          "state_events",
          ConnectionEvent{ConnectionState::Disconnected, session_id});
    }
  }
};

/**
 * @brief Manages outbound, non-blocking TCP connections.
 * @warning This class transmits data in plaintext (cleartext) over raw TCP.
 *          It does NOT provide transport layer security (TLS/SSL). Do not use
 * this for transmitting sensitive or confidential information over public
 * networks without implementing an encryption layer on top of it.
 */
class TcpClient {
 public:
  /**
   * @brief Callback type for handling incoming data from a server.
   * @param session_id The unique session ID of the server connection.
   * @param data The binary payload received.
   */
  using DataHandler = std::function<void(uint64_t session_id,
                                         const std::vector<uint8_t> &data)>;

  /**
   * @brief Constructs a new TcpClient.
   */
  TcpClient()
      : running_(false)
#ifdef CPPTCPNET_SSL_SUPPORT
        ,
        ssl_enabled_(false),
        ssl_ctx_(nullptr)
#endif
  {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
      wsa_initialized_ = true;
    } else {
      Log(LogSeverity::Error, "TcpClient", "WSAStartup failed.");
    }
#endif
  }

  /**
   * @brief Destroys the TcpClient, disconnecting all active connections.
   */
  ~TcpClient() {
    Stop();
#ifdef _WIN32
    if (wsa_initialized_) {
      WSACleanup();
    }
#endif
  }

#ifdef CPPTCPNET_SSL_SUPPORT
  struct SslClientConfig {
    std::string ca_file;
    std::string ca_path;
    bool verify_peer = false;
    std::string client_cert_file;
    std::string client_key_file;
    std::string cipher_list;
    std::string cipher_suites;
    int min_tls_version = -1;
    int max_tls_version = -1;
  };

  void EnableSSL(const SslClientConfig &config) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
      throw std::runtime_error("Cannot enable SSL while client is running.");
    }
    ssl_enabled_ = true;
    ssl_config_ = config;
  }

  void EnableSSL() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
      throw std::runtime_error("Cannot enable SSL while client is running.");
    }
    ssl_enabled_ = true;
    ssl_config_ = SslClientConfig{};
  }
#endif

  void SetNoDelay(bool enabled) {
    no_delay_ = enabled;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.no_delay = enabled;
  }

  bool GetNoDelay() const { return no_delay_; }

  void SetSocketRecvBufferSize(int size) {
    socket_recv_buffer_size_ = size;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.socket_recv_buffer_size = size;
  }

  int GetSocketRecvBufferSize() const { return socket_recv_buffer_size_; }

  void SetSocketSendBufferSize(int size) {
    socket_send_buffer_size_ = size;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.socket_send_buffer_size = size;
  }

  int GetSocketSendBufferSize() const { return socket_send_buffer_size_; }

  void SetKeepAliveConfig(const KeepAliveConfig &config) {
    keepalive_enabled_ = config.enabled;
    keepalive_idle_secs_ = config.idle_secs;
    keepalive_interval_secs_ = config.interval_secs;
    keepalive_count_ = config.count;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.keepalive_enabled = config.enabled;
    default_profile_.keepalive_idle_secs = config.idle_secs;
    default_profile_.keepalive_interval_secs = config.interval_secs;
    default_profile_.keepalive_count = config.count;
  }

  KeepAliveConfig GetKeepAliveConfig() const {
    KeepAliveConfig config;
    config.enabled = keepalive_enabled_;
    config.idle_secs = keepalive_idle_secs_;
    config.interval_secs = keepalive_interval_secs_;
    config.count = keepalive_count_;
    return config;
  }

  void SetLinger(bool enabled, int timeout_secs) {
    linger_enabled_ = enabled;
    linger_timeout_secs_ = timeout_secs;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.linger_enabled = enabled;
    default_profile_.linger_timeout_secs = timeout_secs;
  }

  LingerConfig GetLinger() const {
    LingerConfig config;
    config.enabled = linger_enabled_;
    config.timeout_secs = linger_timeout_secs_;
    return config;
  }

  /**
   * @brief Sets the number of background worker threads for handling callbacks.
   * @param threads The number of background threads.
   * @note Under the hood, this creates a vector of single-threaded worker
   * pools. Incoming data callbacks for any single session are pinned to the
   * same worker pool using a hash of the session ID. This guarantees session
   * affinity (serial, in-order execution of callbacks for any single session)
   *       while allowing concurrent processing across different sessions.
   * @note Must be called before Start(). Not thread-safe to call while the
   * client is running.
   */
  void SetWorkerThreadCount(size_t threads) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
      throw std::runtime_error("Cannot set worker thread count while running.");
    }
    worker_thread_count_ = threads;
  }

  size_t GetWorkerThreadCount() const { return worker_thread_count_; }

  void SetRecvBufferSize(size_t size) {
    recv_buffer_size_ = size;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.recv_buffer_size = size;
  }

  size_t GetRecvBufferSize() const { return recv_buffer_size_; }

  void SetSendChunkSize(size_t size) {
    send_chunk_size_ = size;
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.send_chunk_size = size;
  }

  size_t GetSendChunkSize() const { return send_chunk_size_; }

  void SetSslHandshakeTimeout(std::chrono::milliseconds timeout) {
    ssl_handshake_timeout_ms_ = static_cast<uint32_t>(timeout.count());
  }

  std::chrono::milliseconds GetSslHandshakeTimeout() const {
    return std::chrono::milliseconds(
        ssl_handshake_timeout_ms_.load(std::memory_order_relaxed));
  }

  // Non-copyable and non-movable
  TcpClient(const TcpClient &) = delete;
  TcpClient &operator=(const TcpClient &) = delete;
  TcpClient(TcpClient &&) = delete;
  TcpClient &operator=(TcpClient &&) = delete;

  /**
   * @brief Sets the callback function invoked when data is received from a
   * server.
   * @param handler The data handler function.
   */
  void SetDataHandler(DataHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    data_handler_ = std::move(handler);
  }

  /**
   * @brief Retrieves the PubSub event broker for socket state changes.
   * @return A reference to the PubSub broker.
   */
  cpppubsub::PubSub &GetEventBroker() { return broker_; }

  /**
   * @brief Sets the maximum outbound buffer size per client in bytes.
   * @param max_size The maximum size of the outbound buffer.
   */
  void SetMaxOutboundBufferSize(size_t max_size) {
    max_outbound_buffer_size_.store(max_size, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.max_outbound_buffer_size = max_size;
  }

  /**
   * @brief Gets the maximum outbound buffer size per client in bytes.
   * @return The maximum size.
   */
  size_t GetMaxOutboundBufferSize() const {
    return max_outbound_buffer_size_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Sets the callback function invoked when a background error occurs.
   * @param handler The error handler function.
   */
  void SetErrorHandler(ErrorHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    error_handler_ = std::move(handler);
  }

  /**
   * @brief Sets the connection send/write timeout.
   * @param timeout The duration after which a blocked write is timed out and
   * connection is dropped.
   */
  void SetSendTimeout(std::chrono::milliseconds timeout) {
    send_timeout_ms_.store(static_cast<uint32_t>(timeout.count()),
                           std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.send_timeout = timeout;
  }

  /**
   * @brief Gets the connection send/write timeout.
   * @return The send/write timeout duration.
   */
  std::chrono::milliseconds GetSendTimeout() const {
    return std::chrono::milliseconds(
        send_timeout_ms_.load(std::memory_order_relaxed));
  }

  /**
   * @brief Sets the connection establishment timeout.
   * @param timeout The connection timeout duration.
   */
  void SetConnectTimeout(std::chrono::milliseconds timeout) {
    connect_timeout_ms_.store(static_cast<uint32_t>(timeout.count()),
                              std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.connect_timeout = timeout;
  }

  /**
   * @brief Gets the connection establishment timeout.
   * @return The connection timeout duration.
   */
  std::chrono::milliseconds GetConnectTimeout() const {
    return std::chrono::milliseconds(
        connect_timeout_ms_.load(std::memory_order_relaxed));
  }

  /**
   * @brief Sets the default connection profile for new connections.
   */
  void SetDefaultConnectionProfile(const ConnectionProfile &profile) {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_ = profile;
    // Synchronize individual atomic legacy variables
    no_delay_.store(profile.no_delay, std::memory_order_relaxed);
    socket_recv_buffer_size_.store(profile.socket_recv_buffer_size,
                                   std::memory_order_relaxed);
    socket_send_buffer_size_.store(profile.socket_send_buffer_size,
                                   std::memory_order_relaxed);
    keepalive_enabled_.store(profile.keepalive_enabled,
                             std::memory_order_relaxed);
    keepalive_idle_secs_.store(profile.keepalive_idle_secs,
                               std::memory_order_relaxed);
    keepalive_interval_secs_.store(profile.keepalive_interval_secs,
                                   std::memory_order_relaxed);
    keepalive_count_.store(profile.keepalive_count, std::memory_order_relaxed);
    linger_enabled_.store(profile.linger_enabled, std::memory_order_relaxed);
    linger_timeout_secs_.store(profile.linger_timeout_secs,
                               std::memory_order_relaxed);
    recv_buffer_size_.store(profile.recv_buffer_size,
                            std::memory_order_relaxed);
    send_chunk_size_.store(profile.send_chunk_size, std::memory_order_relaxed);
    max_outbound_buffer_size_.store(profile.max_outbound_buffer_size,
                                    std::memory_order_relaxed);
    idle_timeout_ms_.store(static_cast<uint32_t>(profile.idle_timeout.count()),
                           std::memory_order_relaxed);
    send_timeout_ms_.store(static_cast<uint32_t>(profile.send_timeout.count()),
                           std::memory_order_relaxed);
    connect_timeout_ms_.store(
        static_cast<uint32_t>(profile.connect_timeout.count()),
        std::memory_order_relaxed);
  }

  /**
   * @brief Gets the default connection profile.
   */
  ConnectionProfile GetDefaultConnectionProfile() const {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    return default_profile_;
  }

  /**
   * @brief Applies a connection profile to an active session dynamically
   * on-the-fly.
   */
  void ApplyConnectionProfile(uint64_t session_id,
                              const ConnectionProfile &profile) {
    socket_t fd = INVALID_SOCKET;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = session_to_fd_.find(session_id);
      if (it == session_to_fd_.end()) {
        throw std::runtime_error("Session not found: " +
                                 std::to_string(session_id));
      }
      fd = it->second;
      socket_profiles_[fd] = profile;
    }
    if (fd != INVALID_SOCKET) {
      internal::ApplySocketOptions(fd, profile.ToSocketOptions());
    }
  }

  /**
   * @brief Applies a connection profile to the default connection (for
   * single-connection clients).
   */
  void ApplyConnectionProfile(const ConnectionProfile &profile) {
    uint64_t session_id = 0;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      if (session_to_fd_.size() != 1) {
        throw std::runtime_error(
            "ApplyConnectionProfile without session_id requires exactly one "
            "active connection.");
      }
      session_id = session_to_fd_.begin()->first;
    }
    ApplyConnectionProfile(session_id, profile);
  }

  /**
   * @brief Gets the active connection profile of a specific session.
   */
  ConnectionProfile GetConnectionProfile(uint64_t session_id) const {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    auto it = session_to_fd_.find(session_id);
    if (it == session_to_fd_.end()) {
      throw std::runtime_error("Session not found: " +
                               std::to_string(session_id));
    }
    auto prof_it = socket_profiles_.find(it->second);
    if (prof_it != socket_profiles_.end()) {
      return prof_it->second;
    }
    return default_profile_;
  }

  /**
   * @brief Starts the client's event loop on a background thread.
   * @throws std::runtime_error If the client is already running.
   */
  void Start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    StartInternal();
  }

  /**
   * @brief Stops the client and closes all active connections.
   */
  void Stop(std::chrono::milliseconds drain_timeout) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!running_) return;

    auto start_time = std::chrono::steady_clock::now();
    while (drain_timeout.count() > 0) {
      bool all_drained = true;
      {
        std::lock_guard<std::mutex> out_lock(outbound_mutex_);
        for (const auto &pair : outbound_buffers_) {
          if (pair.second.total_bytes > 0) {
            all_drained = false;
            break;
          }
        }
      }
      if (all_drained) {
        break;
      }
      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (elapsed >= drain_timeout) {
        break;
      }
      wakeup_channel_.Trigger();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    StopInternalLocked();
  }

  void Stop() { Stop(std::chrono::milliseconds(0)); }

 private:
  void StopInternalLocked() {
    if (!running_) return;

    running_ = false;
    cv_.notify_all();
    wakeup_channel_.Trigger();
    if (loop_thread_.joinable()) {
      if (loop_thread_.get_id() == std::this_thread::get_id()) {
        loop_thread_.detach();
      } else {
        loop_thread_.join();
      }
    }

    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
#ifdef CPPTCPNET_SSL_SUPPORT
      for (auto &pair : fd_to_ssl_) {
        SSL_shutdown(pair.second);
        SSL_free(pair.second);
      }
      fd_to_ssl_.clear();
      ssl_handshaking_fds_.clear();
      ssl_read_wants_read_.clear();
      ssl_read_wants_write_.clear();
      ssl_write_wants_read_.clear();
      ssl_write_wants_write_.clear();
      ssl_handshake_start_times_.clear();
#endif
      for (auto &pfd : poll_fds_) {
        if (pfd.fd != INVALID_SOCKET && pfd.fd != wakeup_channel_.ReadFd())
          CLOSE_SOCKET(pfd.fd);
      }
      poll_fds_.clear();
      connecting_sockets_.clear();
      connection_start_times_.clear();
      last_write_time_.clear();
      last_activity_time_.clear();
      fd_targets_.clear();
      reconnecting_targets_.clear();
      fd_to_session_.clear();
      session_to_fd_.clear();
    }
    {
      std::lock_guard<std::mutex> out_lock(outbound_mutex_);
      outbound_buffers_.clear();
    }
#ifdef CPPTCPNET_SSL_SUPPORT
    if (ssl_ctx_) {
      SSL_CTX_free(ssl_ctx_);
      ssl_ctx_ = nullptr;
    }
#endif
    wakeup_channel_.Close();
    worker_pools_.clear();

    std::vector<std::shared_ptr<ReconnectThreadInfo>> threads_to_join;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      threads_to_join = reconnect_threads_;
      reconnect_threads_.clear();
    }
    for (auto &info : threads_to_join) {
      if (info->thread.joinable()) {
        info->thread.join();
      }
    }
  }

  /**
   * @brief Connects to a remote server.
   * @param ip_address The IP address of the server.
   * @param port The port number of the server.
   * @return The unique session ID of the connection if successful.
   * @throws std::system_error If the connection fails.
   */
 public:
  uint64_t Connect(const std::string &ip_address, uint16_t port) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!running_) StartInternal();

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    std::string port_str = std::to_string(port);
    int s = getaddrinfo(ip_address.c_str(), port_str.c_str(), &hints, &res);
    if (s != 0) {
      std::string err_msg = "getaddrinfo failed for " + ip_address + " error: ";
#ifdef _WIN32
      err_msg += std::to_string(WSAGetLastError());
#else
      err_msg += gai_strerror(s);
#endif
      Log(LogSeverity::Error, "TcpClient", err_msg);
      throw std::runtime_error(err_msg);
    }

    socket_t fd = INVALID_SOCKET;
    struct addrinfo *rp = nullptr;
    int last_err = 0;

    for (rp = res; rp != nullptr; rp = rp->ai_next) {
      fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (fd == INVALID_SOCKET) {
        last_err = GetLastSocketError();
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        socket_profiles_[fd] = default_profile_;
        ApplySocketOptions(fd);
      }

      if (!SetNonBlocking(fd)) {
        last_err = GetLastSocketError();
        CLOSE_SOCKET(fd);
        fd = INVALID_SOCKET;
        continue;
      }

      int result = connect(fd, rp->ai_addr, static_cast<int>(rp->ai_addrlen));
      if (result < 0) {
        int err = GetLastSocketError();
        if (err != IN_PROGRESS_ERROR && err != WOULD_BLOCK_ERROR) {
          last_err = err;
          CLOSE_SOCKET(fd);
          fd = INVALID_SOCKET;
          continue;
        }
      }

      break;  // Successfully initiated connection
    }

    freeaddrinfo(res);

    if (fd == INVALID_SOCKET) {
      Log(LogSeverity::Error, "TcpClient",
          "Failed to connect to: " + ip_address);
      throw std::system_error(last_err, std::system_category(),
                              "Failed to connect");
    }

    uint64_t session_id = 0;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      poll_fds_.push_back({fd, static_cast<short>(POLLIN | POLLOUT), 0});
      connecting_sockets_.insert(fd);
      connection_start_times_[fd] = std::chrono::steady_clock::now();
      last_write_time_[fd] = std::chrono::steady_clock::now();
      last_activity_time_[fd] = std::chrono::steady_clock::now();
      fd_targets_[fd] = Target{ip_address, port};
      do {
        session_id = internal::GenerateRandomSessionId();
      } while (session_to_fd_.find(session_id) != session_to_fd_.end());
      fd_to_session_[fd] = session_id;
      session_to_fd_[session_id] = fd;
      cv_.notify_all();
    }

    return session_id;
  }

  /**
   * @brief Sends binary data to a connected server using its session ID.
   * @param session_id The unique session ID of the server connection.
   * @param request The data payload to send.
   * @return True if successfully queued, false otherwise.
   */
  bool Send(uint64_t session_id, const std::vector<uint8_t> &request) {
    return SendImpl(session_id, request);
  }

  bool Send(uint64_t session_id, const std::string &request) {
    return SendImpl(session_id, request);
  }

  bool Send(uint64_t session_id, std::vector<uint8_t> &&request) {
    return SendImpl(session_id, std::move(request));
  }

  bool Send(uint64_t session_id, std::string &&request) {
    return SendImpl(session_id, std::move(request));
  }

  bool Send(uint64_t session_id,
            std::shared_ptr<const std::vector<uint8_t>> request) {
    return SendImpl(session_id, std::move(request));
  }

  bool Send(uint64_t session_id, std::shared_ptr<const std::string> request) {
    return SendImpl(session_id, std::move(request));
  }

  bool Send(const std::vector<uint8_t> &request) {
    return SendNoSessionImpl(request);
  }

  bool Send(const std::string &request) { return SendNoSessionImpl(request); }

  bool Send(std::vector<uint8_t> &&request) {
    return SendNoSessionImpl(std::move(request));
  }

  bool Send(std::string &&request) {
    return SendNoSessionImpl(std::move(request));
  }

  bool Send(std::shared_ptr<const std::vector<uint8_t>> request) {
    return SendNoSessionImpl(std::move(request));
  }

  bool Send(std::shared_ptr<const std::string> request) {
    return SendNoSessionImpl(std::move(request));
  }

  /**
   * @brief Retrieves a snapshot of the current client statistics.
   * @return A ClientStats struct.
   */
  ClientStats GetStats() const {
    ClientStats stats;
    stats.bytes_sent = bytes_sent_.load(std::memory_order_relaxed);
    stats.bytes_received = bytes_received_.load(std::memory_order_relaxed);
    stats.packets_sent = packets_sent_.load(std::memory_order_relaxed);
    stats.packets_received = packets_received_.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      stats.is_connected =
          !fd_to_session_.empty() && connecting_sockets_.empty();
    }
    return stats;
  }

  /**
   * @brief Checks if the client event loop is currently running.
   * @return True if running, false otherwise.
   */
  bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

  /**
   * @brief Programmatically disconnects a connection by its session ID.
   * @param session_id The unique session ID of the server connection.
   * @return True if connection was found and disconnect initiated, false
   * otherwise.
   */
  bool Disconnect(uint64_t session_id) {
    socket_t fd = INVALID_SOCKET;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = session_to_fd_.find(session_id);
      if (it != session_to_fd_.end()) {
        fd = it->second;
        fd_targets_.erase(fd);
      }
    }
    if (fd != INVALID_SOCKET) {
      DisconnectServer(fd);
      return true;
    }
    return false;
  }

  /**
   * @brief Enumerates the currently active session IDs.
   * @return A vector of active session IDs.
   */
  std::vector<uint64_t> GetActiveSessions() const {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    std::vector<uint64_t> sessions;
    sessions.reserve(session_to_fd_.size());
    for (const auto &pair : session_to_fd_) {
      sessions.push_back(pair.first);
    }
    return sessions;
  }

  /**
   * @brief Retrieves the peer address (IP and port) of an active session.
   * @param session_id The session ID to query.
   * @return The PeerAddress struct.
   * @throws std::runtime_error If the session is not found or inactive.
   */
  PeerAddress GetPeerAddress(uint64_t session_id) const {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    auto it = session_to_fd_.find(session_id);
    if (it == session_to_fd_.end()) {
      throw std::runtime_error("Session not found: " +
                               std::to_string(session_id));
    }
    return internal::GetPeerAddressForSocket(it->second);
  }

  /**
   * @brief Sets the connection idle timeout.
   * @param timeout The duration after which an inactive connection is closed.
   */
  void SetIdleTimeout(std::chrono::milliseconds timeout) {
    idle_timeout_ms_.store(static_cast<uint32_t>(timeout.count()),
                           std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(poll_mutex_);
    default_profile_.idle_timeout = timeout;
  }

  /**
   * @brief Gets the connection idle timeout.
   * @return The idle timeout duration.
   */
  std::chrono::milliseconds GetIdleTimeout() const {
    return std::chrono::milliseconds(
        idle_timeout_ms_.load(std::memory_order_relaxed));
  }

  /**
   * @brief Configures the auto-reconnect behavior for disconnected clients.
   * @param enabled Set to true to enable auto-reconnect, false to disable.
   * @param initial_delay The initial wait time before attempting reconnection.
   * @param max_delay The maximum delay limit for exponential backoff.
   */
  void SetAutoReconnect(
      bool enabled,
      std::chrono::milliseconds initial_delay = std::chrono::milliseconds(1000),
      std::chrono::milliseconds max_delay = std::chrono::milliseconds(30000)) {
    auto_reconnect_enabled_.store(enabled, std::memory_order_relaxed);
    reconnect_initial_delay_ = initial_delay;
    reconnect_max_delay_ = max_delay;
  }

  /**
   * @brief Checks if auto-reconnect is enabled.
   * @return True if enabled, false otherwise.
   */
  bool GetAutoReconnectEnabled() const {
    return auto_reconnect_enabled_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Gets the initial reconnect delay duration.
   * @return The initial delay.
   */
  std::chrono::milliseconds GetAutoReconnectInitialDelay() const {
    return reconnect_initial_delay_;
  }

  /**
   * @brief Gets the maximum reconnect delay duration.
   * @return The maximum delay.
   */
  std::chrono::milliseconds GetAutoReconnectMaxDelay() const {
    return reconnect_max_delay_;
  }

 private:
  struct Target {
    std::string ip;
    uint16_t port;
    bool operator==(const Target &o) const {
      return ip == o.ip && port == o.port;
    }
  };
  struct TargetHash {
    size_t operator()(const Target &t) const {
      size_t seed = std::hash<std::string>{}(t.ip);
      seed ^= std::hash<uint16_t>{}(t.port) + 0x9e3779b9 + (seed << 6) +
              (seed >> 2);
      return seed;
    }
  };

  std::atomic<bool> auto_reconnect_enabled_{false};
  std::chrono::milliseconds reconnect_initial_delay_{1000};
  std::chrono::milliseconds reconnect_max_delay_{30000};
  std::unordered_set<Target, TargetHash> reconnecting_targets_;
  std::unordered_map<socket_t, Target> fd_targets_;
  struct ReconnectThreadInfo {
    std::thread thread;
    std::atomic<bool> finished{false};
  };
  std::vector<std::shared_ptr<ReconnectThreadInfo>> reconnect_threads_;

  std::atomic<uint32_t> idle_timeout_ms_{60000};
  std::unordered_map<socket_t, std::chrono::steady_clock::time_point>
      last_activity_time_;

  std::atomic<bool> running_;
  std::thread loop_thread_;
  std::vector<pollfd_t> poll_fds_;
  // Lock ordering convention: Always acquire poll_mutex_ BEFORE outbound_mutex_
  // to prevent deadlocks.
  mutable std::mutex poll_mutex_;

  std::unordered_set<socket_t> connecting_sockets_;
  std::unordered_map<socket_t, std::chrono::steady_clock::time_point>
      connection_start_times_;
  template <typename T>
  bool SendImpl(uint64_t session_id, T &&payload) {
    size_t payload_size = internal::GetDataSize(payload);
    if (payload_size == 0) {
      return true;
    }

    std::lock_guard<std::mutex> poll_lock(poll_mutex_);
    auto it = session_to_fd_.find(session_id);
    if (it == session_to_fd_.end()) {
      Log(LogSeverity::Warning, "TcpClient",
          "Send called on inactive or stale session_id: " +
              std::to_string(session_id));
      return false;
    }
    socket_t fd = it->second;
    auto prof_it = socket_profiles_.find(fd);
    ConnectionProfile prof = (prof_it != socket_profiles_.end())
                                 ? prof_it->second
                                 : default_profile_;

    {
      std::lock_guard<std::mutex> out_lock(outbound_mutex_);
      auto &out_buf = outbound_buffers_[fd];
      if (out_buf.total_bytes + payload_size > prof.max_outbound_buffer_size) {
        Log(LogSeverity::Error, "TcpClient",
            "Outbound buffer limit reached for socket fd: " +
                std::to_string(fd) + ". Dropping data.");
        return false;
      }
      out_buf.chunks.emplace_back(
          internal::OutboundChunk{std::forward<T>(payload), 0});
      out_buf.total_bytes += payload_size;
    }

    bool is_handshaking = false;
#ifdef CPPTCPNET_SSL_SUPPORT
    if (ssl_enabled_) {
      is_handshaking =
          ssl_handshaking_fds_.find(fd) != ssl_handshaking_fds_.end();
    }
#endif
    if (!is_handshaking) {
      for (auto &pfd : poll_fds_) {
        if (pfd.fd == fd) {
          pfd.events |= POLLOUT;
          break;
        }
      }
    }
    wakeup_channel_.Trigger();
    return true;
  }

  template <typename T>
  bool SendNoSessionImpl(T &&request) {
    uint64_t session_id = 0;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      if (session_to_fd_.size() != 1) {
        Log(LogSeverity::Error, "TcpClient",
            "Send without session_id requires exactly one active connection.");
        return false;
      }
      session_id = session_to_fd_.begin()->first;
    }
    return Send(session_id, std::forward<T>(request));
  }

  std::unordered_map<socket_t, internal::OutboundBuffer> outbound_buffers_;
  // Lock ordering convention: Always acquire poll_mutex_ BEFORE outbound_mutex_
  // to prevent deadlocks.
  std::mutex outbound_mutex_;
  std::atomic<size_t> max_outbound_buffer_size_ =
      10 * 1024 * 1024;  // 10MB default
  std::mutex lifecycle_mutex_;
#ifdef _WIN32
  bool wsa_initialized_ = false;
#endif

  mutable std::mutex handler_mutex_;
  DataHandler data_handler_;
  ErrorHandler error_handler_;
  cpppubsub::PubSub broker_;
  std::vector<std::unique_ptr<cppasyncworker::WorkerPool>> worker_pools_;
  std::atomic<size_t> worker_thread_count_{0};
  std::atomic<size_t> recv_buffer_size_{4096};
  std::atomic<size_t> send_chunk_size_{65536};
  std::atomic<uint32_t> ssl_handshake_timeout_ms_{10000};

  internal::WakeUpChannel wakeup_channel_;
  std::unordered_map<socket_t, uint64_t> fd_to_session_;
  std::unordered_map<uint64_t, socket_t> session_to_fd_;
  std::unordered_map<socket_t, std::chrono::steady_clock::time_point>
      last_write_time_;
  std::atomic<uint32_t> send_timeout_ms_{30000};     // 30 seconds default
  std::atomic<uint32_t> connect_timeout_ms_{10000};  // 10 seconds default
  std::condition_variable cv_;

  std::atomic<uint64_t> bytes_sent_{0};
  std::atomic<uint64_t> bytes_received_{0};
  std::atomic<uint64_t> packets_sent_{0};
  std::atomic<uint64_t> packets_received_{0};

#ifdef CPPTCPNET_SSL_SUPPORT
  bool ssl_enabled_ = false;
  SslClientConfig ssl_config_;
  SSL_CTX *ssl_ctx_ = nullptr;
  std::unordered_map<socket_t, SSL *> fd_to_ssl_;
  std::unordered_set<socket_t> ssl_handshaking_fds_;
  std::unordered_map<socket_t, std::chrono::steady_clock::time_point>
      ssl_handshake_start_times_;
  // Per-operation SSL want states to avoid cross-contamination
  std::unordered_set<socket_t> ssl_read_wants_read_;
  std::unordered_set<socket_t> ssl_read_wants_write_;
  std::unordered_set<socket_t> ssl_write_wants_read_;
  std::unordered_set<socket_t> ssl_write_wants_write_;
#endif

  ConnectionProfile default_profile_;
  std::unordered_map<socket_t, ConnectionProfile> socket_profiles_;

  std::atomic<bool> no_delay_{false};
  std::atomic<int> socket_recv_buffer_size_{0};
  std::atomic<int> socket_send_buffer_size_{0};
  std::atomic<bool> keepalive_enabled_{true};
  std::atomic<int> keepalive_idle_secs_{-1};
  std::atomic<int> keepalive_interval_secs_{-1};
  std::atomic<int> keepalive_count_{-1};
  std::atomic<bool> linger_enabled_{false};
  std::atomic<int> linger_timeout_secs_{0};

  void ApplySocketOptions(socket_t fd) {
    // Assumes poll_mutex_ is held
    auto it = socket_profiles_.find(fd);
    ConnectionProfile prof =
        (it != socket_profiles_.end()) ? it->second : default_profile_;
    internal::ApplySocketOptions(fd, prof.ToSocketOptions());
  }

  // Assumes lifecycle_mutex_ is held
  void StartInternal() {
    if (running_) {
      Log(LogSeverity::Error, "TcpClient", "Client is already running.");
      throw std::runtime_error("[TcpClient] Client is already running.");
    }
#ifdef CPPTCPNET_SSL_SUPPORT
    if (ssl_enabled_) {
      internal::InitOpenSSL();
      ssl_ctx_ = SSL_CTX_new(TLS_client_method());
      if (!ssl_ctx_) {
        throw std::runtime_error("Failed to create SSL context.");
      }
      if (ssl_config_.min_tls_version >= 0) {
        SSL_CTX_set_min_proto_version(ssl_ctx_, ssl_config_.min_tls_version);
      } else {
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
      }
      if (ssl_config_.max_tls_version >= 0) {
        SSL_CTX_set_max_proto_version(ssl_ctx_, ssl_config_.max_tls_version);
      }
      if (!ssl_config_.cipher_list.empty()) {
        SSL_CTX_set_cipher_list(ssl_ctx_, ssl_config_.cipher_list.c_str());
      }
      if (!ssl_config_.cipher_suites.empty()) {
        SSL_CTX_set_ciphersuites(ssl_ctx_, ssl_config_.cipher_suites.c_str());
      }
      if (ssl_config_.verify_peer) {
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
        if (!ssl_config_.ca_file.empty() || !ssl_config_.ca_path.empty()) {
          const char *ca_file = ssl_config_.ca_file.empty()
                                    ? nullptr
                                    : ssl_config_.ca_file.c_str();
          const char *ca_path = ssl_config_.ca_path.empty()
                                    ? nullptr
                                    : ssl_config_.ca_path.c_str();
          if (SSL_CTX_load_verify_locations(ssl_ctx_, ca_file, ca_path) <= 0) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
            throw std::runtime_error("Failed to load CA verify locations.");
          }
        } else {
          SSL_CTX_set_default_verify_paths(ssl_ctx_);
        }
      }
      if (!ssl_config_.client_cert_file.empty() &&
          !ssl_config_.client_key_file.empty()) {
        if (SSL_CTX_use_certificate_chain_file(
                ssl_ctx_, ssl_config_.client_cert_file.c_str()) <= 0) {
          SSL_CTX_free(ssl_ctx_);
          ssl_ctx_ = nullptr;
          throw std::runtime_error("Failed to load client certificate: " +
                                   ssl_config_.client_cert_file);
        }
        if (SSL_CTX_use_PrivateKey_file(ssl_ctx_,
                                        ssl_config_.client_key_file.c_str(),
                                        SSL_FILETYPE_PEM) <= 0) {
          SSL_CTX_free(ssl_ctx_);
          ssl_ctx_ = nullptr;
          throw std::runtime_error("Failed to load client private key: " +
                                   ssl_config_.client_key_file);
        }
        if (SSL_CTX_check_private_key(ssl_ctx_) <= 0) {
          SSL_CTX_free(ssl_ctx_);
          ssl_ctx_ = nullptr;
          throw std::runtime_error(
              "Client private key does not match certificate.");
        }
      }
    }
#endif
    if (!wakeup_channel_.Initialize()) {
      Log(LogSeverity::Error, "TcpClient",
          "Failed to initialize WakeUpChannel");
      throw std::runtime_error("Failed to initialize WakeUpChannel");
    }
    size_t threads = worker_thread_count_.load(std::memory_order_relaxed);
    if (threads == 0) {
      threads = std::thread::hardware_concurrency();
    }
    if (threads == 0) {
      threads = 1;
    }
    worker_pools_.resize(threads);
    for (size_t i = 0; i < threads; ++i) {
      worker_pools_[i] = std::make_unique<cppasyncworker::WorkerPool>(1);
    }

    running_ = true;
    try {
      loop_thread_ = std::thread(&TcpClient::RunPollLoop, this);
    } catch (...) {
      running_ = false;
      worker_pools_.clear();
      wakeup_channel_.Close();
      Log(LogSeverity::Error, "TcpClient", "Failed to start poll loop thread.");
      throw;
    }
  }

  void ReportError(int error_code, const std::string &message) {
    Log(LogSeverity::Error, "TcpClient", message);
    ErrorHandler err_handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      err_handler = error_handler_;
    }
    if (err_handler && !worker_pools_.empty()) {
      (void)worker_pools_[0]->Enqueue([err_handler, error_code, message]() {
        try {
          err_handler(error_code, message);
        } catch (const std::exception &e) {
          Log(LogSeverity::Error, "TcpClient",
              "Exception in user error handler: " + std::string(e.what()));
        } catch (...) {
          Log(LogSeverity::Error, "TcpClient",
              "Unknown exception in user error handler");
        }
      });
    }
    broker_.Publish("error_events", ErrorEvent{error_code, message});
  }

  void RunPollLoop() {
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      poll_fds_.push_back({wakeup_channel_.ReadFd(), POLLIN, 0});
    }

    while (running_) {
      std::vector<pollfd_t> poll_fds_copy;
      {
        std::unique_lock<std::mutex> lock(poll_mutex_);
        cv_.wait(lock, [this]() { return poll_fds_.size() > 1 || !running_; });
        if (!running_) break;
        poll_fds_copy = poll_fds_;
      }

      // Check connection timeouts
      std::vector<socket_t> timed_out;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto connect_timeout_duration = std::chrono::milliseconds(
            connect_timeout_ms_.load(std::memory_order_relaxed));
        for (auto s_fd : connecting_sockets_) {
          auto it = connection_start_times_.find(s_fd);
          if (it != connection_start_times_.end() &&
              (now - it->second) > connect_timeout_duration) {
            timed_out.push_back(s_fd);
          }
        }
      }
      for (auto s_fd : timed_out) {
        ReportError(TIMEOUT_ERROR_CODE, "Connection timed out asynchronously.");
        DisconnectServer(s_fd);
      }

#ifdef CPPTCPNET_SSL_SUPPORT
      // Check SSL handshake timeouts
      std::vector<socket_t> ssl_handshake_timeout_fds;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto ssl_timeout_duration = std::chrono::milliseconds(
            ssl_handshake_timeout_ms_.load(std::memory_order_relaxed));
        if (ssl_timeout_duration.count() > 0) {
          for (auto fd : ssl_handshaking_fds_) {
            auto it = ssl_handshake_start_times_.find(fd);
            if (it != ssl_handshake_start_times_.end() &&
                now - it->second > ssl_timeout_duration) {
              ssl_handshake_timeout_fds.push_back(fd);
            }
          }
        }
      }
      for (auto fd : ssl_handshake_timeout_fds) {
        Log(LogSeverity::Warning, "TcpClient",
            "Closing connection on fd " + std::to_string(fd) +
                " due to SSL handshake timeout.");
        ReportError(TIMEOUT_ERROR_CODE,
                    "SSL handshake timed out asynchronously.");
        DisconnectServer(fd);
      }
#endif

      // Check idle timeouts
      std::vector<socket_t> idle_connections;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (const auto &pair : last_activity_time_) {
          socket_t fd = pair.first;
          auto prof_it = socket_profiles_.find(fd);
          auto timeout_duration =
              (prof_it != socket_profiles_.end())
                  ? prof_it->second.idle_timeout
                  : std::chrono::milliseconds(
                        idle_timeout_ms_.load(std::memory_order_relaxed));
          if (timeout_duration.count() > 0 &&
              now - pair.second > timeout_duration) {
            idle_connections.push_back(fd);
          }
        }
      }
      for (auto client_fd : idle_connections) {
        Log(LogSeverity::Warning, "TcpClient",
            "Closing idle connection on fd " + std::to_string(client_fd));
        ReportError(TIMEOUT_ERROR_CODE,
                    "Connection timed out due to idle timeout.");
        DisconnectServer(client_fd);
      }

      // Check send timeouts
      std::vector<socket_t> send_timed_out_connections;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        std::lock_guard<std::mutex> out_lock(outbound_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto &pair : last_write_time_) {
          socket_t fd = pair.first;
          if (connecting_sockets_.find(fd) != connecting_sockets_.end()) {
            continue;  // Don't apply send timeout while connecting
          }
          auto it = outbound_buffers_.find(fd);
          bool has_pending =
              (it != outbound_buffers_.end() && it->second.total_bytes > 0);
          if (has_pending) {
            auto prof_it = socket_profiles_.find(fd);
            auto send_timeout_duration =
                (prof_it != socket_profiles_.end())
                    ? prof_it->second.send_timeout
                    : std::chrono::milliseconds(
                          send_timeout_ms_.load(std::memory_order_relaxed));
            if (send_timeout_duration.count() > 0 &&
                now - pair.second > send_timeout_duration) {
              send_timed_out_connections.push_back(fd);
            }
          } else {
            pair.second = now;
          }
        }
      }
      for (auto fd : send_timed_out_connections) {
        ReportError(TIMEOUT_ERROR_CODE, "Send timed out asynchronously.");
        DisconnectServer(fd);
      }

      int poll_count;
      do {
#ifdef _WIN32
        poll_count = POLL_FUNC(poll_fds_copy.data(),
                               static_cast<ULONG>(poll_fds_copy.size()), 1000);
#else
        poll_count =
            POLL_FUNC(poll_fds_copy.data(), poll_fds_copy.size(), 1000);
#endif
      } while (poll_count < 0 && GetLastSocketError() == INTR_ERROR &&
               running_);

      if (poll_count < 0) {
        if (running_) {
          ReportError(GetLastSocketError(), "Poll error.");
        }
        break;
      }
      if (poll_count == 0) continue;

      for (size_t i = 0; i < poll_fds_copy.size(); ++i) {
        if (poll_fds_copy[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) {
          if (poll_fds_copy[i].fd == wakeup_channel_.ReadFd()) {
            if (poll_fds_copy[i].revents & POLLIN) wakeup_channel_.Clear();
          } else {
            ProcessServerEvents(poll_fds_copy[i]);
          }
        }
      }
    }
  }

#ifdef CPPTCPNET_SSL_SUPPORT
  void HandleSslHandshake(socket_t fd, uint64_t session_id, bool &disconnect) {
    SSL *ssl = nullptr;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = fd_to_ssl_.find(fd);
      if (it == fd_to_ssl_.end()) return;
      ssl = it->second;
    }

    int ret = SSL_connect(ssl);
    if (ret <= 0) {
      int err = SSL_get_error(ssl, ret);
      if (err == SSL_ERROR_WANT_READ) {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        ssl_read_wants_read_.insert(fd);
        ssl_read_wants_write_.erase(fd);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == fd) {
            master_pfd.events = POLLIN;
            break;
          }
        }
      } else if (err == SSL_ERROR_WANT_WRITE) {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        ssl_read_wants_write_.insert(fd);
        ssl_read_wants_read_.erase(fd);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == fd) {
            master_pfd.events = POLLOUT;
            break;
          }
        }
      } else {
        disconnect = true;
        unsigned long oscode = ERR_get_error();
        char errbuf[256];
        ERR_error_string_n(oscode, errbuf, sizeof(errbuf));
        Log(LogSeverity::Warning, "TcpClient",
            "SSL handshake failed: " + std::string(errbuf));
      }
    } else {
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        ssl_handshaking_fds_.erase(fd);
        ssl_handshake_start_times_.erase(fd);
        ssl_read_wants_read_.erase(fd);
        ssl_read_wants_write_.erase(fd);
        ssl_write_wants_read_.erase(fd);
        ssl_write_wants_write_.erase(fd);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == fd) {
            master_pfd.events = POLLIN;
            break;
          }
        }
      }
      Log(LogSeverity::Info, "TcpClient",
          "SSL Handshake completed for session: " + std::to_string(session_id));
      broker_.Publish("state_events",
                      ConnectionEvent{ConnectionState::Connected, session_id});

      bool needs_pollout = false;
      {
        std::lock_guard<std::mutex> out_lock(outbound_mutex_);
        auto it = outbound_buffers_.find(fd);
        if (it != outbound_buffers_.end() && it->second.total_bytes > 0) {
          needs_pollout = true;
        }
      }
      if (needs_pollout) {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        for (auto &master_pfd : poll_fds_) {
          if (master_pfd.fd == fd) {
            master_pfd.events |= POLLOUT;
            break;
          }
        }
      }
    }
  }
#endif

  void ProcessServerEvents(const pollfd_t &pfd) {
    socket_t fd = pfd.fd;
    bool disconnect = false;

    uint64_t session_id = 0;
    ConnectionProfile prof;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = fd_to_session_.find(fd);
      if (it != fd_to_session_.end()) {
        session_id = it->second;
      }
      auto prof_it = socket_profiles_.find(fd);
      prof = (prof_it != socket_profiles_.end()) ? prof_it->second
                                                 : default_profile_;
    }

    // Check if it's currently connecting
    bool is_connecting = false;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      is_connecting = connecting_sockets_.find(fd) != connecting_sockets_.end();
    }

    if (is_connecting) {
      int error = 0;
      socklen_t len = sizeof(error);
      // Portable reinterpret_cast to char* is used here to support both POSIX
      // and Windows getsockopt() signatures.
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error),
                     &len) < 0) {
        error = GetLastSocketError();
      }

      if (error != 0) {
        disconnect = true;
        ReportError(error, "Connection failed asynchronously.");
      } else {
        // Successfully connected
#ifdef CPPTCPNET_SSL_SUPPORT
        if (ssl_enabled_) {
          SSL *ssl = SSL_new(ssl_ctx_);
          if (!ssl) {
            disconnect = true;
            ReportError(0, "Failed to create SSL object.");
          } else {
            SSL_set_fd(ssl, static_cast<int>(fd));
            SSL_set_connect_state(ssl);
            {
              std::lock_guard<std::mutex> lock(poll_mutex_);
              fd_to_ssl_[fd] = ssl;
              ssl_handshaking_fds_.insert(fd);
              ssl_handshake_start_times_[fd] = std::chrono::steady_clock::now();
              connecting_sockets_.erase(fd);
              connection_start_times_.erase(fd);
              last_write_time_[fd] = std::chrono::steady_clock::now();
              last_activity_time_[fd] = std::chrono::steady_clock::now();
              for (auto &master_pfd : poll_fds_) {
                if (master_pfd.fd == fd) {
                  master_pfd.events = POLLIN | POLLOUT;
                  break;
                }
              }
            }
          }
        } else {
          {
            std::lock_guard<std::mutex> lock(poll_mutex_);
            connecting_sockets_.erase(fd);
            connection_start_times_.erase(fd);
            last_write_time_[fd] = std::chrono::steady_clock::now();
            last_activity_time_[fd] = std::chrono::steady_clock::now();

            for (auto &master_pfd : poll_fds_) {
              if (master_pfd.fd == fd) {
                master_pfd.events =
                    POLLIN;  // Remove POLLOUT until Send is called
                break;
              }
            }
          }
          broker_.Publish(
              "state_events",
              ConnectionEvent{ConnectionState::Connected, session_id});
        }
#else
        {
          std::lock_guard<std::mutex> lock(poll_mutex_);
          connecting_sockets_.erase(fd);
          connection_start_times_.erase(fd);
          last_write_time_[fd] = std::chrono::steady_clock::now();
          last_activity_time_[fd] = std::chrono::steady_clock::now();

          for (auto &master_pfd : poll_fds_) {
            if (master_pfd.fd == fd) {
              master_pfd.events =
                  POLLIN;  // Remove POLLOUT until Send is called
              break;
            }
          }
        }
        broker_.Publish(
            "state_events",
            ConnectionEvent{ConnectionState::Connected, session_id});
#endif

        // Proceed to handle existing POLLOUT if data was buffered while
        // connecting
        if (!disconnect) {
          bool needs_pollout = false;
          {
            std::lock_guard<std::mutex> out_lock(outbound_mutex_);
            auto it = outbound_buffers_.find(fd);
            if (it != outbound_buffers_.end() && it->second.total_bytes > 0) {
              needs_pollout = true;
            }
          }
          if (needs_pollout) {
            std::lock_guard<std::mutex> poll_lock(poll_mutex_);
            for (auto &master_pfd : poll_fds_) {
              if (master_pfd.fd == fd) {
                master_pfd.events |= POLLOUT;
                break;
              }
            }
          }
        }
      }
    } else {
#ifdef CPPTCPNET_SSL_SUPPORT
      bool is_handshaking = false;
      SSL *ssl = nullptr;
      {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        is_handshaking =
            ssl_handshaking_fds_.find(fd) != ssl_handshaking_fds_.end();
        auto it = fd_to_ssl_.find(fd);
        if (it != fd_to_ssl_.end()) ssl = it->second;
      }

      if (ssl_enabled_ && is_handshaking) {
        HandleSslHandshake(fd, session_id, disconnect);
        if (disconnect) {
          DisconnectServer(fd);
        }
        return;
      }

      if (ssl_enabled_ && ssl) {
        bool should_read = false;
        bool should_write = false;
        {
          std::lock_guard<std::mutex> lock(poll_mutex_);
          bool read_wants_write =
              ssl_read_wants_write_.find(fd) != ssl_read_wants_write_.end();
          bool write_wants_read =
              ssl_write_wants_read_.find(fd) != ssl_write_wants_read_.end();

          if ((pfd.revents & POLLIN) && !read_wants_write) {
            should_read = true;
          }
          if ((pfd.revents & POLLOUT) && read_wants_write) {
            should_read = true;
          }
          if ((pfd.revents & POLLOUT) && !write_wants_read) {
            should_write = true;
          }
          if ((pfd.revents & POLLIN) && write_wants_read) {
            should_write = true;
          }
        }

        if (should_read) {
          bool read_more = true;
          while (read_more && !disconnect && running_) {
            size_t buf_size = prof.recv_buffer_size;
            std::vector<char> dynamic_buf;
            char stack_buf[4096];
            char *buffer = stack_buf;
            if (buf_size > 4096) {
              dynamic_buf.resize(buf_size);
              buffer = dynamic_buf.data();
            }
            int bytes_read = 0;
            int err = 0;
            while (running_) {
              bytes_read = SSL_read(ssl, buffer, static_cast<int>(buf_size));
              if (bytes_read >= 0) break;
              err = SSL_get_error(ssl, bytes_read);
              if (err == SSL_ERROR_SYSCALL && GET_SOCKET_ERROR == INTR_ERROR)
                continue;
              break;
            }

            if (bytes_read > 0) {
              {
                std::lock_guard<std::mutex> lock(poll_mutex_);
                ssl_read_wants_read_.erase(fd);
                ssl_read_wants_write_.erase(fd);
                last_activity_time_[fd] = std::chrono::steady_clock::now();
              }
              bytes_received_.fetch_add(bytes_read, std::memory_order_relaxed);
              packets_received_.fetch_add(1, std::memory_order_relaxed);
              broker_.Publish(
                  "transfer_events",
                  TransferEvent{session_id, static_cast<size_t>(bytes_read),
                                false});

              std::vector<uint8_t> payload(buffer, buffer + bytes_read);

              DataHandler handler;
              {
                std::lock_guard<std::mutex> lock(handler_mutex_);
                handler = data_handler_;
              }
              if (handler && !worker_pools_.empty()) {
                size_t pool_idx = session_id % worker_pools_.size();
                (void)worker_pools_[pool_idx]->Enqueue(
                    [session_id, payload = std::move(payload),
                     handler = std::move(handler)]() {
                      try {
                        handler(session_id, payload);
                      } catch (const std::exception &e) {
                        Log(LogSeverity::Error, "TcpClient",
                            "Exception in user data handler: " +
                                std::string(e.what()));
                      } catch (...) {
                        Log(LogSeverity::Error, "TcpClient",
                            "Unknown exception in user data handler");
                      }
                    });
              }
            } else if (bytes_read == 0) {
              disconnect = true;
              read_more = false;
            } else {
              read_more = false;
              if (err == SSL_ERROR_WANT_READ) {
                std::lock_guard<std::mutex> lock(poll_mutex_);
                ssl_read_wants_read_.insert(fd);
                ssl_read_wants_write_.erase(fd);
              } else if (err == SSL_ERROR_WANT_WRITE) {
                std::lock_guard<std::mutex> lock(poll_mutex_);
                ssl_read_wants_write_.insert(fd);
                ssl_read_wants_read_.erase(fd);
              } else {
                disconnect = true;
              }
            }
          }
        }

        size_t sent_bytes_to_publish = 0;
        if (!disconnect && should_write) {
          std::lock_guard<std::mutex> poll_lock(poll_mutex_);
          std::lock_guard<std::mutex> out_lock(outbound_mutex_);
          auto it = outbound_buffers_.find(fd);
          if (it != outbound_buffers_.end()) {
            auto &out_buf = it->second;
            if (out_buf.total_bytes > 0) {
              auto &chunk = out_buf.chunks.front();
              size_t remaining = chunk.remaining();
              size_t to_send = (std::min)(remaining, prof.send_chunk_size);
              int sent = 0;
              int err = 0;
              while (running_) {
                sent = SSL_write(ssl, chunk.ptr(), static_cast<int>(to_send));
                if (sent > 0) break;
                err = SSL_get_error(ssl, sent);
                if (err == SSL_ERROR_SYSCALL && GET_SOCKET_ERROR == INTR_ERROR)
                  continue;
                break;
              }

              if (sent > 0) {
                ssl_write_wants_read_.erase(fd);
                ssl_write_wants_write_.erase(fd);
                bytes_sent_.fetch_add(sent, std::memory_order_relaxed);
                packets_sent_.fetch_add(1, std::memory_order_relaxed);
                sent_bytes_to_publish = static_cast<size_t>(sent);
                last_write_time_[fd] = std::chrono::steady_clock::now();
                last_activity_time_[fd] = std::chrono::steady_clock::now();
                chunk.offset += sent;
                out_buf.total_bytes -= sent;
                if (chunk.offset == chunk.size()) {
                  out_buf.chunks.pop_front();
                }
              } else {
                if (err == SSL_ERROR_WANT_READ) {
                  ssl_write_wants_read_.insert(fd);
                  ssl_write_wants_write_.erase(fd);
                } else if (err == SSL_ERROR_WANT_WRITE) {
                  ssl_write_wants_write_.insert(fd);
                  ssl_write_wants_read_.erase(fd);
                } else {
                  disconnect = true;
                }
              }
            }

            if (out_buf.total_bytes == 0 && ssl_write_wants_write_.find(fd) ==
                                                ssl_write_wants_write_.end()) {
              last_write_time_[fd] = std::chrono::steady_clock::now();
            }
          }
        }
        if (sent_bytes_to_publish > 0) {
          broker_.Publish(
              "transfer_events",
              TransferEvent{session_id, sent_bytes_to_publish, true});
        }

        if (!disconnect) {
          std::lock_guard<std::mutex> lock(poll_mutex_);
          for (auto &master_pfd : poll_fds_) {
            if (master_pfd.fd == fd) {
              short events = 0;
              bool has_outbound = false;
              {
                std::lock_guard<std::mutex> out_lock(outbound_mutex_);
                auto it = outbound_buffers_.find(fd);
                if (it != outbound_buffers_.end() &&
                    it->second.total_bytes > 0) {
                  has_outbound = true;
                }
              }

              bool need_pollin =
                  ssl_read_wants_read_.find(fd) != ssl_read_wants_read_.end() ||
                  ssl_write_wants_read_.find(fd) !=
                      ssl_write_wants_read_.end() ||
                  (ssl_read_wants_write_.find(fd) ==
                   ssl_read_wants_write_.end());
              if (need_pollin) {
                events |= POLLIN;
              }
              if (has_outbound ||
                  ssl_read_wants_write_.find(fd) !=
                      ssl_read_wants_write_.end() ||
                  ssl_write_wants_write_.find(fd) !=
                      ssl_write_wants_write_.end()) {
                events |= POLLOUT;
              }
              master_pfd.events = events;
              break;
            }
          }
        }

        if (disconnect) {
          DisconnectServer(fd);
        }
        return;
      }
#endif

      if (pfd.revents & POLLIN) {
        size_t buf_size = prof.recv_buffer_size;
        std::vector<char> dynamic_buf;
        char stack_buf[4096];
        char *buffer = stack_buf;
        if (buf_size > 4096) {
          dynamic_buf.resize(buf_size);
          buffer = dynamic_buf.data();
        }
        ssize_t bytes_read;
        do {
          bytes_read =
              recv(fd, buffer, static_cast<socket_buf_size_t>(buf_size), 0);
        } while (bytes_read < 0 && GetLastSocketError() == INTR_ERROR &&
                 running_);

        if (bytes_read > 0) {
          {
            std::lock_guard<std::mutex> lock(poll_mutex_);
            last_activity_time_[fd] = std::chrono::steady_clock::now();
          }
          bytes_received_.fetch_add(bytes_read, std::memory_order_relaxed);
          packets_received_.fetch_add(1, std::memory_order_relaxed);
          broker_.Publish(
              "transfer_events",
              TransferEvent{session_id, static_cast<size_t>(bytes_read),
                            false});

          std::vector<uint8_t> payload(buffer, buffer + bytes_read);

          DataHandler handler;
          {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler = data_handler_;
          }
          if (handler && !worker_pools_.empty()) {
            size_t pool_idx = session_id % worker_pools_.size();
            (void)worker_pools_[pool_idx]->Enqueue(
                [session_id, payload = std::move(payload),
                 handler = std::move(handler)]() {
                  try {
                    handler(session_id, payload);
                  } catch (const std::exception &e) {
                    Log(LogSeverity::Error, "TcpClient",
                        "Exception in user data handler: " +
                            std::string(e.what()));
                  } catch (...) {
                    Log(LogSeverity::Error, "TcpClient",
                        "Unknown exception in user data handler");
                  }
                });
          }
        } else if (bytes_read == 0) {
          disconnect = true;
        } else if (bytes_read < 0) {
          if (int err = GetLastSocketError();
              err != IN_PROGRESS_ERROR && err != WOULD_BLOCK_ERROR) {
            disconnect = true;
          }
        }
      }

      size_t sent_bytes_to_publish = 0;
      if (!disconnect && (pfd.revents & POLLOUT)) {
        std::lock_guard<std::mutex> poll_lock(poll_mutex_);
        std::lock_guard<std::mutex> out_lock(outbound_mutex_);
        auto it = outbound_buffers_.find(fd);
        if (it != outbound_buffers_.end()) {
          auto &out_buf = it->second;
          if (out_buf.total_bytes > 0) {
            auto &chunk = out_buf.chunks.front();
            size_t remaining = chunk.remaining();
            size_t to_send = (std::min)(remaining, prof.send_chunk_size);
            ssize_t sent;
            do {
              sent = send(fd, reinterpret_cast<const char *>(chunk.ptr()),
                          static_cast<socket_buf_size_t>(to_send), SEND_FLAGS);
            } while (sent < 0 && GetLastSocketError() == INTR_ERROR &&
                     running_);

            if (sent > 0) {
              bytes_sent_.fetch_add(sent, std::memory_order_relaxed);
              packets_sent_.fetch_add(1, std::memory_order_relaxed);
              sent_bytes_to_publish = static_cast<size_t>(sent);
              last_write_time_[fd] = std::chrono::steady_clock::now();
              last_activity_time_[fd] = std::chrono::steady_clock::now();
              chunk.offset += sent;
              out_buf.total_bytes -= sent;
              if (chunk.offset == chunk.size()) {
                out_buf.chunks.pop_front();
              }
            } else if (sent < 0) {
              if (int err = GetLastSocketError();
                  err != IN_PROGRESS_ERROR && err != WOULD_BLOCK_ERROR) {
                disconnect = true;
              }
            }
          }

          if (out_buf.total_bytes == 0) {
            last_write_time_[fd] = std::chrono::steady_clock::now();
            for (auto &master_pfd : poll_fds_) {
              if (master_pfd.fd == fd) {
                master_pfd.events &= ~POLLOUT;
                break;
              }
            }
          }
        }
      }
      if (sent_bytes_to_publish > 0) {
        broker_.Publish("transfer_events",
                        TransferEvent{session_id, sent_bytes_to_publish, true});
      }

      if (!disconnect && (pfd.revents & (POLLERR | POLLHUP))) {
        disconnect = true;
      }
    }

    if (disconnect) {
      DisconnectServer(fd);
    }
  }

  void DisconnectServer(socket_t fd) {
#ifdef CPPTCPNET_SSL_SUPPORT
    SSL *ssl = nullptr;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      auto it = fd_to_ssl_.find(fd);
      if (it != fd_to_ssl_.end()) {
        ssl = it->second;
        fd_to_ssl_.erase(it);
      }
      ssl_handshaking_fds_.erase(fd);
      ssl_handshake_start_times_.erase(fd);
      ssl_read_wants_read_.erase(fd);
      ssl_read_wants_write_.erase(fd);
      ssl_write_wants_read_.erase(fd);
      ssl_write_wants_write_.erase(fd);
    }
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
    }
#endif
    uint64_t session_id = 0;
    Target target;
    bool has_target = false;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      connecting_sockets_.erase(fd);
      connection_start_times_.erase(fd);
      socket_profiles_.erase(fd);
      last_write_time_.erase(fd);
      last_activity_time_.erase(fd);

      auto it_target = fd_targets_.find(fd);
      if (it_target != fd_targets_.end()) {
        target = it_target->second;
        has_target = true;
        fd_targets_.erase(it_target);
      }

      auto it_sess = fd_to_session_.find(fd);
      if (it_sess != fd_to_session_.end()) {
        session_id = it_sess->second;
        fd_to_session_.erase(it_sess);
        session_to_fd_.erase(session_id);
      }
      for (auto it = poll_fds_.begin(); it != poll_fds_.end(); ++it) {
        if (it->fd == fd) {
          poll_fds_.erase(it);
          break;
        }
      }
    }
    {
      std::lock_guard<std::mutex> out_lock(outbound_mutex_);
      outbound_buffers_.erase(fd);
    }
    CLOSE_SOCKET(fd);
    if (session_id != 0) {
      broker_.Publish(
          "state_events",
          ConnectionEvent{ConnectionState::Disconnected, session_id});
    }

    if (has_target && auto_reconnect_enabled_.load(std::memory_order_relaxed) &&
        running_) {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      if (reconnecting_targets_.find(target) == reconnecting_targets_.end()) {
        reconnecting_targets_.insert(target);

        // Clean up finished reconnect threads first
        for (auto it = reconnect_threads_.begin();
             it != reconnect_threads_.end();) {
          if ((*it)->finished.load()) {
            if ((*it)->thread.joinable()) {
              (*it)->thread.join();
            }
            it = reconnect_threads_.erase(it);
          } else {
            ++it;
          }
        }

        auto info = std::make_shared<ReconnectThreadInfo>();
        info->thread = std::thread([this, target, info]() {
          std::chrono::milliseconds current_delay = reconnect_initial_delay_;
          while (running_) {
            auto sleep_start = std::chrono::steady_clock::now();
            while (running_) {
              if (std::chrono::steady_clock::now() - sleep_start >=
                  current_delay) {
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!running_) break;

            uint64_t new_session = 0;
            bool connect_initiated = false;
            try {
              new_session = Connect(target.ip, target.port);
              connect_initiated = true;
            } catch (const std::exception &e) {
              Log(LogSeverity::Warning, "TcpClient",
                  "Auto-reconnect Connect call failed: " +
                      std::string(e.what()));
            }

            if (connect_initiated) {
              bool resolved = false;
              bool success = false;
              while (running_ && !resolved) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::lock_guard<std::mutex> lock_state(poll_mutex_);
                auto it = session_to_fd_.find(new_session);
                if (it == session_to_fd_.end()) {
                  resolved = true;
                  success = false;
                } else {
                  socket_t new_fd = it->second;
                  if (connecting_sockets_.find(new_fd) ==
                      connecting_sockets_.end()) {
                    resolved = true;
                    success = true;
                  }
                }
              }

              if (!running_) break;

              if (success) {
                Log(LogSeverity::Info, "TcpClient",
                    "Auto-reconnect succeeded for target " + target.ip + ":" +
                        std::to_string(target.port));
                std::lock_guard<std::mutex> lock_state(poll_mutex_);
                reconnecting_targets_.erase(target);
                info->finished.store(true);
                return;
              }
            }

            current_delay = (std::min)(current_delay * 2, reconnect_max_delay_);
          }

          std::lock_guard<std::mutex> lock_state(poll_mutex_);
          reconnecting_targets_.erase(target);
          info->finished.store(true);
        });
        reconnect_threads_.push_back(info);
      }
    }
  }
};

/**
 * @brief Utility class to track sliding-window network throughput using PubSub
 * TransferEvents.
 */
class ThroughputTracker {
 public:
  /**
   * @brief Constructs a ThroughputTracker.
   * @param broker The PubSub event broker to subscribe to.
   * @param window_duration The sliding-window duration over which to compute
   * throughput.
   */
  ThroughputTracker(
      cpppubsub::PubSub &broker,
      std::chrono::milliseconds window_duration = std::chrono::seconds(1))
      : window_duration_(window_duration),
        total_send_bytes_(0),
        total_recv_bytes_(0) {
    sub_ = broker.Subscribe<TransferEvent>(
        "transfer_events", 10000, cpppubsub::OverflowPolicy::DropOldest);
    worker_.AddSubscription<TransferEvent>(
        sub_, [this](const TransferEvent &event) {
          std::lock_guard<std::mutex> lock(mutex_);
          auto now = std::chrono::steady_clock::now();
          samples_.push_back({now, event.bytes_transferred, event.is_send});
          if (event.is_send)
            total_send_bytes_ += event.bytes_transferred;
          else
            total_recv_bytes_ += event.bytes_transferred;
          CleanOldSamples(now);
        });
    worker_.Start();
  }

  /**
   * @brief Destructs the ThroughputTracker.
   */
  ~ThroughputTracker() { worker_.Stop(); }

  // Non-copyable and non-movable
  ThroughputTracker(const ThroughputTracker &) = delete;
  ThroughputTracker &operator=(const ThroughputTracker &) = delete;
  ThroughputTracker(ThroughputTracker &&) = delete;
  ThroughputTracker &operator=(ThroughputTracker &&) = delete;

  /**
   * @brief Returns the calculated send throughput in bytes per second.
   * @return The send throughput.
   */
  double GetSendThroughputBytesPerSec() { return CalculateThroughput(true); }

  /**
   * @brief Returns the calculated receive throughput in bytes per second.
   * @return The receive throughput.
   */
  double GetRecvThroughputBytesPerSec() { return CalculateThroughput(false); }

 private:
  struct Sample {
    std::chrono::steady_clock::time_point timestamp;
    size_t bytes;
    bool is_send;
  };

  double CalculateThroughput(bool is_send) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    CleanOldSamples(now);

    size_t total_bytes = is_send ? total_send_bytes_ : total_recv_bytes_;
    double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(
                         window_duration_)
                         .count();
    return seconds > 0 ? (total_bytes / seconds) : 0.0;
  }

  void CleanOldSamples(std::chrono::steady_clock::time_point now) {
    auto threshold = now - window_duration_;
    while (!samples_.empty() && samples_.front().timestamp < threshold) {
      auto &front = samples_.front();
      if (front.is_send) {
        if (total_send_bytes_ >= front.bytes)
          total_send_bytes_ -= front.bytes;
        else
          total_send_bytes_ = 0;
      } else {
        if (total_recv_bytes_ >= front.bytes)
          total_recv_bytes_ -= front.bytes;
        else
          total_recv_bytes_ = 0;
      }
      samples_.pop_front();
    }
  }

  std::chrono::milliseconds window_duration_;
  std::shared_ptr<cpppubsub::Subscriber<TransferEvent>> sub_;
  cpppubsub::Worker worker_;
  std::deque<Sample> samples_;
  size_t total_send_bytes_ = 0;
  size_t total_recv_bytes_ = 0;
  std::mutex mutex_;
};

}  // namespace cpptcpnet