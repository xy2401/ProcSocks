#include "socks.hpp"
#include "l10n.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace ps {
namespace {
constexpr std::size_t max_udp_datagram = 65507;
void append_address(std::vector<std::uint8_t>& bytes, const Endpoint& endpoint) {
    // port() also validates the stored sockaddr length.
    const auto port = endpoint.port();
    if (endpoint.family() == AF_INET) {
        bytes.push_back(1);
        const auto* address = reinterpret_cast<const sockaddr_in*>(&endpoint.address);
        const auto* first = reinterpret_cast<const std::uint8_t*>(&address->sin_addr);
        bytes.insert(bytes.end(), first, first + 4);
    } else if (endpoint.family() == AF_INET6) {
        const auto* address = reinterpret_cast<const sockaddr_in6*>(&endpoint.address);
        if (address->sin6_scope_id) throw std::runtime_error(tr(Msg::RtSocksScope));
        bytes.push_back(4);
        const auto* first = reinterpret_cast<const std::uint8_t*>(&address->sin6_addr);
        bytes.insert(bytes.end(), first, first + 16);
    } else throw std::runtime_error(tr(Msg::RtSocksIp));
    bytes.push_back(static_cast<std::uint8_t>(port >> 8));
    bytes.push_back(static_cast<std::uint8_t>(port & 0xff));
}
std::uint16_t read_port(const std::uint8_t* value) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(value[0]) << 8) | value[1]);
}
Endpoint numeric_address(std::uint8_t type, const std::uint8_t* bytes) {
    if (type == 1) {
        auto endpoint = Endpoint::any(AF_INET, read_port(bytes + 4));
        std::memcpy(&reinterpret_cast<sockaddr_in*>(&endpoint.address)->sin_addr, bytes, 4);
        return endpoint;
    }
    if (type == 4) {
        auto endpoint = Endpoint::any(AF_INET6, read_port(bytes + 16));
        std::memcpy(&reinterpret_cast<sockaddr_in6*>(&endpoint.address)->sin6_addr, bytes, 16);
        return endpoint;
    }
    throw std::runtime_error(tr(Msg::RtSocksAddressType));
}
std::string domain_name(const std::uint8_t* bytes, std::size_t length) {
    if (!length || length > 255) throw std::runtime_error(tr(Msg::RtSocksDomainLength));
    for (std::size_t i = 0; i < length; ++i)
        if (bytes[i] <= 32 || bytes[i] >= 127) throw std::runtime_error(tr(Msg::RtSocksDomain));
    return {reinterpret_cast<const char*>(bytes), length};
}
std::string reply_error(std::uint8_t code) {
    switch (code) {
        case 1: return tr(Msg::RtSocksGeneralFailure);
        case 2: return tr(Msg::RtSocksDenied);
        case 3: return tr(Msg::RtSocksNetworkUnreachable);
        case 4: return tr(Msg::RtSocksHostUnreachable);
        case 5: return tr(Msg::RtSocksRefused);
        case 6: return tr(Msg::RtSocksTtl);
        case 7: return tr(Msg::RtSocksCommandUnsupported);
        case 8: return tr(Msg::RtSocksAddressUnsupported);
        default: return tr(Msg::RtSocksReplyCode);
    }
}
}

Socket socks_authenticated(const ProxyConfig& proxy, Deadline deadline, const std::atomic_bool& stop) {
    if (!proxy.port) throw std::runtime_error(tr(Msg::RtSocksProxyPort));
    if (proxy.authenticate && (proxy.username.empty() || proxy.username.size() > 255 ||
                               proxy.password.empty() || proxy.password.size() > 255))
        throw std::runtime_error(tr(Msg::RtSocksCredentialsLength));
    const auto endpoints = resolve_endpoints(proxy.host, proxy.port, SOCK_STREAM, deadline, stop);
    Socket socket;
    std::string lastError = tr(Msg::RtSocksNoAddress);
    for (const auto& endpoint : endpoints) {
        try {
            // Give alternate DNS addresses a chance within the overall handshake deadline.
            socket = connect_endpoint(endpoint, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(3)), stop);
            break;
        } catch (const std::exception& error) {
            lastError = error.what();
            if (stop.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline) break;
        }
    }
    if (!socket) throw std::runtime_error(tr(Msg::RtSocksConnectFailed, {lastError}));
    const auto method = static_cast<std::uint8_t>(proxy.authenticate ? 2 : 0);
    const std::array<std::uint8_t, 3> greeting{5, 1, method};
    send_all(socket.get(), greeting.data(), greeting.size(), deadline, stop);
    std::array<std::uint8_t, 2> response{};
    receive_exact(socket.get(), response.data(), response.size(), deadline, stop);
    if (response[0] != 5) throw std::runtime_error(tr(Msg::RtSocksGreeting));
    if (response[1] != method) throw std::runtime_error(tr(Msg::RtSocksMethod));
    if (proxy.authenticate) {
        std::vector<std::uint8_t> authentication;
        authentication.reserve(3 + proxy.username.size() + proxy.password.size());
        authentication.push_back(1);
        authentication.push_back(static_cast<std::uint8_t>(proxy.username.size()));
        authentication.insert(authentication.end(), proxy.username.begin(), proxy.username.end());
        authentication.push_back(static_cast<std::uint8_t>(proxy.password.size()));
        authentication.insert(authentication.end(), proxy.password.begin(), proxy.password.end());
        send_all(socket.get(), authentication.data(), authentication.size(), deadline, stop);
        receive_exact(socket.get(), response.data(), response.size(), deadline, stop);
        if (response[0] != 1 || response[1] != 0) throw std::runtime_error(tr(Msg::RtSocksAuthentication));
    }
    return socket;
}

Endpoint socks_command(SOCKET socket, std::uint8_t command, const Endpoint& target, Deadline deadline, const std::atomic_bool& stop) {
    if (command != 1 && command != 3) throw std::runtime_error(tr(Msg::RtSocksCommand));
    if (command == 1 && !target.port()) throw std::runtime_error(tr(Msg::RtSocksDestinationPort));
    std::vector<std::uint8_t> request{5, command, 0};
    append_address(request, target);
    send_all(socket, request.data(), request.size(), deadline, stop);
    std::array<std::uint8_t, 4> header{};
    receive_exact(socket, header.data(), header.size(), deadline, stop);
    if (header[0] != 5 || header[2] != 0) throw std::runtime_error(tr(Msg::RtSocksReplyHeader));
    if (header[1] != 0) throw std::runtime_error(tr(Msg::RtSocksCommandFailed, {reply_error(header[1])}));
    // Consume only protocol bytes: any application data coalesced into the same TCP
    // segment remains queued on the socket for the caller's forwarding loop.
    if (header[3] == 1 || header[3] == 4) {
        std::array<std::uint8_t, 18> address{};
        receive_exact(socket, address.data(), header[3] == 1 ? 6 : 18, deadline, stop);
        return numeric_address(header[3], address.data());
    }
    if (header[3] == 3) {
        std::uint8_t length = 0;
        receive_exact(socket, &length, 1, deadline, stop);
        if (!length) throw std::runtime_error(tr(Msg::RtSocksEmptyDomain));
        std::array<std::uint8_t, 257> address{};
        receive_exact(socket, address.data(), static_cast<std::size_t>(length) + 2, deadline, stop);
        const auto name = domain_name(address.data(), length);
        const auto port = read_port(address.data() + length);
        const auto family = socket_peer(socket).family();
        // CONNECT has already succeeded. BND.ADDR identifies the server's bind
        // address, not a destination to contact, and may name a private host that
        // the client cannot resolve. Our forwarding path does not use it; retain
        // the port in an unspecified placeholder without issuing a DNS query.
        if (command == 1) return Endpoint::any(family, port);
        const auto endpoints = resolve_endpoints(name, port, SOCK_DGRAM, deadline, stop);
        auto preferred = std::find_if(endpoints.begin(), endpoints.end(), [family](const Endpoint& endpoint) { return endpoint.family() == family; });
        return preferred == endpoints.end() ? endpoints.front() : *preferred;
    }
    throw std::runtime_error(tr(Msg::RtSocksReplyAddress));
}

Socket socks_connect(const ProxyConfig& proxy, const Endpoint& target, Deadline deadline, const std::atomic_bool& stop) {
    auto socket = socks_authenticated(proxy, deadline, stop);
    socks_command(socket.get(), 1, target, deadline, stop);
    return socket;
}

std::vector<std::uint8_t> encode_udp(const Endpoint& target, const void* data, std::size_t length) {
    if (length && !data) throw std::runtime_error(tr(Msg::RtUdpNullPayload));
    if (!target.port()) throw std::runtime_error(tr(Msg::RtUdpDestinationPort));
    std::vector<std::uint8_t> packet{0, 0, 0};
    append_address(packet, target);
    if (length > max_udp_datagram - packet.size()) throw std::runtime_error(tr(Msg::RtSocksUdpTooLarge));
    if (length) {
        const auto* first = static_cast<const std::uint8_t*>(data);
        packet.insert(packet.end(), first, first + length);
    }
    return packet;
}

UdpPacket decode_udp(const void* data, std::size_t length) {
    if (!data || length < 4 || length > max_udp_datagram) throw std::runtime_error(tr(Msg::RtSocksUdpSize));
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (bytes[0] || bytes[1]) throw std::runtime_error(tr(Msg::RtSocksUdpReserved));
    if (bytes[2]) throw std::runtime_error(tr(Msg::RtSocksUdpFragment));
    UdpPacket packet;
    std::size_t consumed = 0;
    if (bytes[3] == 1 || bytes[3] == 4) {
        consumed = bytes[3] == 1 ? 10 : 22;
        if (length < consumed) throw std::runtime_error(tr(Msg::RtSocksUdpTruncatedAddress));
        packet.remote = numeric_address(bytes[3], bytes + 4);
    } else if (bytes[3] == 3) {
        if (length < 5 || !bytes[4]) throw std::runtime_error(tr(Msg::RtSocksUdpDomainLength));
        consumed = 7 + static_cast<std::size_t>(bytes[4]);
        if (length < consumed) throw std::runtime_error(tr(Msg::RtSocksUdpTruncatedDomain));
        const auto name = domain_name(bytes + 5, bytes[4]);
        const auto endpoints = resolve_endpoints(name, read_port(bytes + 5 + bytes[4]), SOCK_DGRAM);
        packet.remote = endpoints.front();
    } else throw std::runtime_error(tr(Msg::RtSocksUdpAddressType));
    if (!packet.remote.port()) throw std::runtime_error(tr(Msg::RtSocksUdpSourcePort));
    packet.payload.assign(bytes + consumed, bytes + length);
    return packet;
}
}
