#include "net.hpp"
#include "l10n.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

// MinGW's ws2tcpip.h declares GetAddrInfoExW but omits these Windows 8 APIs.
extern "C" WINSOCK_API_LINKAGE INT WSAAPI GetAddrInfoExCancel(LPHANDLE);
extern "C" WINSOCK_API_LINKAGE INT WSAAPI GetAddrInfoExOverlappedResult(LPOVERLAPPED);

namespace ps {
namespace {
[[noreturn]] void socket_error(const std::string& operation, int error = WSAGetLastError()) {
    throw std::runtime_error(tr(Msg::RtWinsockFailure, {operation, std::to_string(error)}));
}
void check_deadline(Deadline deadline, const std::atomic_bool& stop) {
    if (stop.load(std::memory_order_relaxed)) throw std::runtime_error(tr(Msg::RtCancelled));
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(tr(Msg::RtNetworkTimeout));
}
void validate(const Endpoint& endpoint) {
    if ((endpoint.family() == AF_INET && endpoint.length == sizeof(sockaddr_in)) ||
        (endpoint.family() == AF_INET6 && endpoint.length == sizeof(sockaddr_in6))) return;
    throw std::runtime_error(tr(Msg::RtInvalidEndpoint));
}
bool retryable(int error) { return error == WSAEWOULDBLOCK || error == WSAEINTR; }
bool bypass_v4(const unsigned char* ip, bool privateNetworks) {
    if (ip[0] == 127) return true;
    if (!privateNetworks) return false;
    return ip[0] == 10 || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) ||
           (ip[0] == 192 && ip[1] == 168) || (ip[0] == 169 && ip[1] == 254) ||
           (ip[0] >= 224 && ip[0] <= 239) ||
           (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255);
}
struct HandleCloser { void operator()(void* handle) const { if (handle) CloseHandle(handle); } };
struct AddressCloser { void operator()(ADDRINFOEXW* value) const { if (value) FreeAddrInfoExW(value); } };
}

int Endpoint::family() const { return address.ss_family; }
std::uint16_t Endpoint::port() const {
    validate(*this);
    if (family() == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
}
void Endpoint::set_port(std::uint16_t value) {
    validate(*this);
    if (family() == AF_INET) reinterpret_cast<sockaddr_in*>(&address)->sin_port = htons(value);
    else reinterpret_cast<sockaddr_in6*>(&address)->sin6_port = htons(value);
}
std::string Endpoint::text() const {
    validate(*this);
    char buffer[INET6_ADDRSTRLEN]{};
    const void* ip = family() == AF_INET
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(&address)->sin_addr)
        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr);
    if (!InetNtopA(family(), const_cast<void*>(ip), buffer, sizeof(buffer))) socket_error("InetNtop");
    std::string host(buffer);
    if (family() == AF_INET6) {
        const auto scope = reinterpret_cast<const sockaddr_in6*>(&address)->sin6_scope_id;
        if (scope) host += "%" + std::to_string(scope);
        host = "[" + host + "]";
    }
    return host + ":" + std::to_string(port());
}
bool Endpoint::unspecified() const {
    validate(*this);
    if (family() == AF_INET) return reinterpret_cast<const sockaddr_in*>(&address)->sin_addr.s_addr == 0;
    const auto& ip = reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr;
    return IN6_IS_ADDR_UNSPECIFIED(&ip);
}
Endpoint Endpoint::from_raw(const void* raw) {
    if (!raw) throw std::runtime_error(tr(Msg::RtNullAddress));
    ADDRESS_FAMILY family{};
    std::memcpy(&family, raw, sizeof(family));
    Endpoint result;
    if (family == AF_INET) result.length = sizeof(sockaddr_in);
    else if (family == AF_INET6) result.length = sizeof(sockaddr_in6);
    else throw std::runtime_error(tr(Msg::RtAddressFamily));
    std::memcpy(&result.address, raw, result.length);
    return result;
}
Endpoint Endpoint::any(int family, std::uint16_t port) {
    Endpoint result;
    if (family == AF_INET) result.length = sizeof(sockaddr_in);
    else if (family == AF_INET6) result.length = sizeof(sockaddr_in6);
    else throw std::runtime_error(tr(Msg::RtAddressFamily));
    result.address.ss_family = static_cast<ADDRESS_FAMILY>(family);
    result.set_port(port);
    return result;
}
Endpoint Endpoint::loopback(int family, std::uint16_t port) {
    auto result = any(family, port);
    if (family == AF_INET) reinterpret_cast<sockaddr_in*>(&result.address)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else reinterpret_cast<sockaddr_in6*>(&result.address)->sin6_addr.s6_addr[15] = 1;
    return result;
}
bool same_endpoint(const Endpoint& a, const Endpoint& b) {
    validate(a); validate(b);
    if (a.family() != b.family() || a.port() != b.port()) return false;
    if (a.family() == AF_INET)
        return reinterpret_cast<const sockaddr_in*>(&a.address)->sin_addr.s_addr ==
               reinterpret_cast<const sockaddr_in*>(&b.address)->sin_addr.s_addr;
    const auto* x = reinterpret_cast<const sockaddr_in6*>(&a.address);
    const auto* y = reinterpret_cast<const sockaddr_in6*>(&b.address);
    return x->sin6_scope_id == y->sin6_scope_id && std::memcmp(&x->sin6_addr, &y->sin6_addr, 16) == 0;
}
bool bypass_address(const Endpoint& endpoint, bool privateNetworks) {
    validate(endpoint);
    if (endpoint.family() == AF_INET) {
        const auto* ip = reinterpret_cast<const unsigned char*>(&reinterpret_cast<const sockaddr_in*>(&endpoint.address)->sin_addr);
        return bypass_v4(ip, privateNetworks);
    }
    const auto& ip = reinterpret_cast<const sockaddr_in6*>(&endpoint.address)->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(&ip)) return bypass_v4(ip.s6_addr + 12, privateNetworks);
    if (IN6_IS_ADDR_LOOPBACK(&ip)) return true;
    return privateNetworks && ((ip.s6_addr[0] & 0xfe) == 0xfc ||
        (ip.s6_addr[0] == 0xfe && (ip.s6_addr[1] & 0xc0) == 0x80) || ip.s6_addr[0] == 0xff);
}

std::vector<Endpoint> resolve_endpoints(const std::string& host, std::uint16_t port, int socketType) {
    const std::atomic_bool stop{false};
    return resolve_endpoints(host, port, socketType, std::chrono::steady_clock::now() + std::chrono::seconds(5), stop);
}
std::vector<Endpoint> resolve_endpoints(const std::string& host, std::uint16_t port, int socketType, Deadline deadline, const std::atomic_bool& stop) {
    check_deadline(deadline, stop);
    if (host.empty() || host.size() > 1024 || host.find('\0') != std::string::npos)
        throw std::runtime_error(tr(Msg::RtHostname));
    if (socketType != SOCK_STREAM && socketType != SOCK_DGRAM) throw std::runtime_error(tr(Msg::RtSocketType));
    // Numeric addresses need no resolver, including when no DNS is configured.
    for (int family : {AF_INET, AF_INET6}) {
        auto endpoint = Endpoint::any(family, port);
        void* ip = family == AF_INET
            ? static_cast<void*>(&reinterpret_cast<sockaddr_in*>(&endpoint.address)->sin_addr)
            : static_cast<void*>(&reinterpret_cast<sockaddr_in6*>(&endpoint.address)->sin6_addr);
        if (InetPtonA(family, host.c_str(), ip) == 1) return {endpoint};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.data(), static_cast<int>(host.size()), nullptr, 0);
    if (!required) throw std::runtime_error(tr(Msg::RtHostnameEncoding));
    std::wstring name(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.data(), static_cast<int>(host.size()), name.data(), required);
    const auto service = std::to_wstring(port);
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socketType;
    hints.ai_protocol = socketType == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;
    OVERLAPPED overlapped{};
    std::unique_ptr<void, HandleCloser> event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw std::runtime_error(tr(Msg::RtDnsEvent));
    overlapped.hEvent = event.get();
    PADDRINFOEXW raw = nullptr;
    HANDLE cancelHandle = nullptr;
    deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(5));
    timeval timeout{5, 0};
    int status = GetAddrInfoExW(name.c_str(), service.c_str(), NS_DNS, nullptr, &hints, &raw,
                               &timeout, &overlapped, nullptr, &cancelHandle);
    bool aborted = false;
    if (status == WSA_IO_PENDING) {
        for (;;) {
            if (WaitForSingleObject(event.get(), 25) == WAIT_OBJECT_0) break;
            if (stop.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline) {
                aborted = true;
                // Cancellation signals completion. Keep OVERLAPPED/result storage alive until then.
                GetAddrInfoExCancel(&cancelHandle);
                WaitForSingleObject(event.get(), INFINITE);
                break;
            }
        }
        status = GetAddrInfoExOverlappedResult(&overlapped);
    }
    std::unique_ptr<ADDRINFOEXW, AddressCloser> addresses(raw);
    if (aborted) check_deadline(deadline, stop);
    check_deadline(deadline, stop);
    if (status != 0) socket_error(tr(Msg::RtDnsLookup), status);
    std::vector<Endpoint> result;
    for (auto* cursor = raw; cursor; cursor = cursor->ai_next) {
        if (cursor->ai_family != AF_INET && cursor->ai_family != AF_INET6) continue;
        const auto size = cursor->ai_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (!cursor->ai_addr || cursor->ai_addrlen < size) continue;
        auto endpoint = Endpoint::from_raw(cursor->ai_addr);
        if (std::none_of(result.begin(), result.end(), [&](const Endpoint& other) { return same_endpoint(other, endpoint); }))
            result.push_back(endpoint);
        if (result.size() == 32) break;
    }
    if (result.empty()) throw std::runtime_error(tr(Msg::RtNoHostnameAddress));
    return result;
}

Socket::~Socket() { reset(); }
Socket::Socket(Socket&& other) noexcept : value_(other.release()) {}
Socket& Socket::operator=(Socket&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
SOCKET Socket::release() { return std::exchange(value_, INVALID_SOCKET); }
void Socket::reset(SOCKET value) { if (value_ != INVALID_SOCKET) closesocket(value_); value_ = value; }
void nonblocking(SOCKET socket) { u_long enabled = 1; if (ioctlsocket(socket, FIONBIO, &enabled)) socket_error("ioctlsocket"); }
void wait_socket(SOCKET socket, bool writing, Deadline deadline, const std::atomic_bool& stop) {
    for (;;) {
        check_deadline(deadline, stop);
        fd_set pending, errors;
        FD_ZERO(&pending); FD_SET(socket, &pending);
        FD_ZERO(&errors); FD_SET(socket, &errors);
        auto micros = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
        timeval timeout{0, static_cast<long>(std::clamp<std::int64_t>(micros, 1, 50000))};
        const int ready = select(0, writing ? nullptr : &pending, writing ? &pending : nullptr, &errors, &timeout);
        if (ready == SOCKET_ERROR) { if (WSAGetLastError() == WSAEINTR) continue; socket_error("select"); }
        if (ready == 0) continue;
        check_deadline(deadline, stop);
        if (FD_ISSET(socket, &errors)) {
            int error = 0; int size = sizeof(error);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size)) socket_error("getsockopt");
            socket_error("socket", error ? error : WSAECONNABORTED);
        }
        return;
    }
}
void send_all(SOCKET socket, const void* data, std::size_t length, Deadline deadline, const std::atomic_bool& stop) {
    if (length && !data) throw std::runtime_error(tr(Msg::RtNullSend));
    nonblocking(socket);
    auto* cursor = static_cast<const char*>(data);
    while (length) {
        check_deadline(deadline, stop);
        const auto amount = static_cast<int>(std::min(length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int sent = send(socket, cursor, amount, 0);
        if (sent == SOCKET_ERROR) {
            if (!retryable(WSAGetLastError())) socket_error("send");
            wait_socket(socket, true, deadline, stop); continue;
        }
        if (!sent) throw std::runtime_error(tr(Msg::RtSendClosed));
        cursor += sent; length -= static_cast<std::size_t>(sent);
    }
}
void receive_exact(SOCKET socket, void* data, std::size_t length, Deadline deadline, const std::atomic_bool& stop) {
    if (length && !data) throw std::runtime_error(tr(Msg::RtNullReceive));
    nonblocking(socket);
    auto* cursor = static_cast<char*>(data);
    while (length) {
        check_deadline(deadline, stop);
        const auto amount = static_cast<int>(std::min(length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int received = recv(socket, cursor, amount, 0);
        if (received == SOCKET_ERROR) {
            if (!retryable(WSAGetLastError())) socket_error("recv");
            wait_socket(socket, false, deadline, stop); continue;
        }
        if (!received) throw std::runtime_error(tr(Msg::RtReceiveClosed));
        cursor += received; length -= static_cast<std::size_t>(received);
    }
}
Socket connect_endpoint(const Endpoint& endpoint, Deadline deadline, const std::atomic_bool& stop) {
    validate(endpoint); check_deadline(deadline, stop);
    Socket result(::socket(endpoint.family(), SOCK_STREAM, IPPROTO_TCP));
    if (!result) socket_error("socket");
    if (endpoint.family() == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<const sockaddr_in6*>(&endpoint.address)->sin6_addr)) {
        // Windows creates IPv6-only sockets by default. A mapped address still
        // needs an explicitly dual-stack socket, including for proxy.host.
        const DWORD v6Only = 0;
        if (setsockopt(result.get(), IPPROTO_IPV6, IPV6_V6ONLY,
                       reinterpret_cast<const char*>(&v6Only), sizeof(v6Only))) socket_error(tr(Msg::RtMappedConnect));
    }
    nonblocking(result.get());
    if (connect(result.get(), reinterpret_cast<const sockaddr*>(&endpoint.address), endpoint.length) == SOCKET_ERROR) {
        const auto error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) socket_error("connect", error);
        wait_socket(result.get(), true, deadline, stop);
        int status = 0; int length = sizeof(status);
        if (getsockopt(result.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &length)) socket_error("getsockopt");
        if (status) socket_error("connect", status);
    }
    return result;
}
Endpoint socket_local(SOCKET socket) {
    Endpoint endpoint; endpoint.length = sizeof(endpoint.address);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&endpoint.address), &endpoint.length)) socket_error("getsockname");
    validate(endpoint); return endpoint;
}
Endpoint socket_peer(SOCKET socket) {
    Endpoint endpoint; endpoint.length = sizeof(endpoint.address);
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&endpoint.address), &endpoint.length)) socket_error("getpeername");
    validate(endpoint); return endpoint;
}
}
