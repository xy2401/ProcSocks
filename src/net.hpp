#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
namespace ps {
using Deadline = std::chrono::steady_clock::time_point;
struct Endpoint {
    sockaddr_storage address{};
    int length = 0;
    int family() const;
    std::uint16_t port() const;
    void set_port(std::uint16_t value);
    std::string text() const;
    bool unspecified() const;
    static Endpoint from_raw(const void* address);
    static Endpoint loopback(int family, std::uint16_t port);
    static Endpoint any(int family, std::uint16_t port = 0);
};
bool same_endpoint(const Endpoint& a, const Endpoint& b);
bool bypass_address(const Endpoint& endpoint, bool privateNetworks);
std::vector<Endpoint> resolve_endpoints(const std::string& host, std::uint16_t port, int socketType);
std::vector<Endpoint> resolve_endpoints(const std::string& host, std::uint16_t port, int socketType, Deadline deadline, const std::atomic_bool& stop);
class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET value) : value_(value) {}
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    SOCKET get() const { return value_; }
    explicit operator bool() const { return value_ != INVALID_SOCKET; }
    SOCKET release();
    void reset(SOCKET value = INVALID_SOCKET);
private:
    SOCKET value_ = INVALID_SOCKET;
};
void nonblocking(SOCKET socket);
void wait_socket(SOCKET socket, bool writing, Deadline deadline, const std::atomic_bool& stop);
void send_all(SOCKET socket, const void* data, std::size_t length, Deadline deadline, const std::atomic_bool& stop);
void receive_exact(SOCKET socket, void* data, std::size_t length, Deadline deadline, const std::atomic_bool& stop);
Socket connect_endpoint(const Endpoint& endpoint, Deadline deadline, const std::atomic_bool& stop);
Endpoint socket_local(SOCKET socket);
Endpoint socket_peer(SOCKET socket);
}
