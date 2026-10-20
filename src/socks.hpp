#pragma once
#include "config.hpp"
#include "net.hpp"
namespace ps {
Socket socks_authenticated(const ProxyConfig& proxy, Deadline deadline, const std::atomic_bool& stop);
Endpoint socks_command(SOCKET socket, std::uint8_t command, const Endpoint& target, Deadline deadline, const std::atomic_bool& stop);
Socket socks_connect(const ProxyConfig& proxy, const Endpoint& target, Deadline deadline, const std::atomic_bool& stop);
std::vector<std::uint8_t> encode_udp(const Endpoint& target, const void* data, std::size_t length);
struct UdpPacket { Endpoint remote; std::vector<std::uint8_t> payload; };
UdpPacket decode_udp(const void* data, std::size_t length);
}
