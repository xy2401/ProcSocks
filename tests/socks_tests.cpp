#include "l10n.hpp"
#include "net.hpp"
#include "socks.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template <typename Function> void must_throw(Function function, const std::string& message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("expected failure: " + message);
}
ps::Deadline soon() { return std::chrono::steady_clock::now() + 3s; }
ps::Endpoint endpoint(const std::string& host, std::uint16_t port = 443) {
    return ps::resolve_endpoints(host, port, SOCK_STREAM).front();
}
using Bytes = std::vector<std::uint8_t>;
Bytes read(SOCKET socket, std::size_t count, const std::atomic_bool& stop) {
    Bytes bytes(count);
    ps::receive_exact(socket, bytes.data(), bytes.size(), soon(), stop);
    return bytes;
}
void write(SOCKET socket, const Bytes& bytes, const std::atomic_bool& stop, bool fragment = false) {
    if (fragment) {
        for (auto byte : bytes) {
            ps::send_all(socket, &byte, 1, soon(), stop);
            std::this_thread::sleep_for(1ms);
        }
    } else ps::send_all(socket, bytes.data(), bytes.size(), soon(), stop);
}
class MockServer {
public:
    explicit MockServer(std::function<void(SOCKET, const std::atomic_bool&)> handler, int family = AF_INET) {
        listener_.reset(::socket(family, SOCK_STREAM, IPPROTO_TCP));
        require(static_cast<bool>(listener_), "create test listener");
        const auto address = ps::Endpoint::loopback(family, 0);
        require(bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address.address), address.length) == 0, "bind test listener");
        require(listen(listener_.get(), 1) == 0, "listen test socket");
        ps::nonblocking(listener_.get());
        address_ = ps::socket_local(listener_.get());
        thread_ = std::thread([this, handler] {
            try {
                ps::wait_socket(listener_.get(), false, soon(), stop_);
                ps::Socket client(accept(listener_.get(), nullptr, nullptr));
                require(static_cast<bool>(client), "accept test socket");
                ps::nonblocking(client.get());
                handler(client.get(), stop_);
            } catch (...) { if (!stop_) failure_ = std::current_exception(); }
        });
    }
    ~MockServer() { stop_ = true; if (thread_.joinable()) thread_.join(); }
    ps::ProxyConfig proxy(bool auth = false) const {
        ps::ProxyConfig result;
        result.host = address_.family() == AF_INET ? "127.0.0.1" : "::1";
        result.port = address_.port();
        result.authenticate = auth;
        if (auth) { result.username = "alice"; result.password = "secret"; }
        return result;
    }
    ps::Endpoint address() const { return address_; }
    void finish() { if (thread_.joinable()) thread_.join(); if (failure_) std::rethrow_exception(failure_); }
private:
    ps::Socket listener_;
    ps::Endpoint address_;
    std::atomic_bool stop_{false};
    std::exception_ptr failure_;
    std::thread thread_;
};
void noauth(SOCKET socket, const std::atomic_bool& stop, bool fragment = false) {
    require(read(socket, 3, stop) == Bytes({5, 1, 0}), "no-auth greeting");
    write(socket, {5, 0}, stop, fragment);
}
Bytes command(SOCKET socket, const std::atomic_bool& stop, std::uint8_t expectedCommand = 1) {
    auto header = read(socket, 4, stop);
    require(header[0] == 5 && header[1] == expectedCommand && header[2] == 0, "command header");
    require(header[3] == 1 || header[3] == 4, "command address type");
    auto address = read(socket, header[3] == 1 ? 6 : 18, stop);
    header.insert(header.end(), address.begin(), address.end());
    return header;
}

void test_bypass_and_endpoints() {
    for (const auto& host : {"127.0.0.1", "127.255.255.254", "::1", "::ffff:127.1.2.3"})
        require(ps::bypass_address(endpoint(host), false), std::string("loopback must bypass: ") + host);
    for (const auto& host : {"10.0.0.0", "10.255.255.255", "172.16.0.0", "172.31.255.255", "192.168.1.2", "169.254.0.1", "224.0.0.1", "239.255.255.255", "255.255.255.255", "fc00::1", "fdff::1", "fe80::1", "febf::1", "ff02::1", "::ffff:192.168.1.2"}) {
        require(ps::bypass_address(endpoint(host), true), std::string("private must bypass: ") + host);
        require(!ps::bypass_address(endpoint(host), false), std::string("private bypass switch: ") + host);
    }
    for (const auto& host : {"11.0.0.1", "172.15.255.255", "172.32.0.0", "192.169.0.1", "169.253.255.255", "223.255.255.255", "240.0.0.1", "fbff::1", "fec0::1", "2001:db8::1"})
        require(!ps::bypass_address(endpoint(host), true), std::string("public must proxy: ") + host);
    auto v4 = endpoint("192.0.2.123", 1234);
    require(v4.text() == "192.0.2.123:1234", "IPv4 text");
    require(ps::same_endpoint(v4, ps::Endpoint::from_raw(&v4.address)), "raw round trip");
    require(!ps::same_endpoint(v4, endpoint("192.0.2.123", 4321)), "port identity");
    auto v6 = endpoint("2001:db8::1", 1234);
    require(v6.text() == "[2001:db8::1]:1234", "IPv6 text");
    auto scoped = v6;
    reinterpret_cast<sockaddr_in6*>(&scoped.address)->sin6_scope_id = 7;
    require(!ps::same_endpoint(v6, scoped), "IPv6 scope identity");
    require(ps::Endpoint::any(AF_INET).unspecified() && ps::Endpoint::any(AF_INET6).unspecified(), "unspecified addresses");
    must_throw([] { ps::Endpoint{}.port(); }, "invalid endpoint");
}

void test_udp() {
    const Bytes data{0, 1, 2, 0xff, 0, 7};
    for (const auto& host : {"203.0.113.22", "2001:db8::22"}) {
        auto target = endpoint(host, 53);
        auto wire = ps::encode_udp(target, data.data(), data.size());
        auto decoded = ps::decode_udp(wire.data(), wire.size());
        require(ps::same_endpoint(target, decoded.remote) && decoded.payload == data, "UDP binary round trip");
        for (std::size_t size = 0; size < wire.size() - data.size(); ++size)
            must_throw([&] { ps::decode_udp(wire.data(), size); }, "truncated UDP header");
        auto empty = ps::encode_udp(target, nullptr, 0);
        require(ps::decode_udp(empty.data(), empty.size()).payload.empty(), "empty UDP payload");
    }
    auto wire = ps::encode_udp(endpoint("203.0.113.2", 53), data.data(), data.size());
    for (auto index : {0, 1, 2}) {
        auto bad = wire; bad[index] = 1;
        must_throw([&] { ps::decode_udp(bad.data(), bad.size()); }, "reserved/fragment fields");
    }
    wire[3] = 7;
    must_throw([&] { ps::decode_udp(wire.data(), wire.size()); }, "invalid address type");
    Bytes domain{0, 0, 0, 3, 9, '1', '2', '7', '.', '0', '.', '0', '.', '1', 0, 53, 42};
    auto decoded = ps::decode_udp(domain.data(), domain.size());
    require(ps::same_endpoint(decoded.remote, endpoint("127.0.0.1", 53)) && decoded.payload == Bytes{42}, "UDP domain address");
    domain[4] = 0;
    must_throw([&] { ps::decode_udp(domain.data(), domain.size()); }, "empty UDP domain");
    domain[4] = 255;
    must_throw([&] { ps::decode_udp(domain.data(), domain.size()); }, "truncated UDP domain");
    Bytes maximum(65507 - 10, 8);
    require(ps::encode_udp(endpoint("192.0.2.1", 9), maximum.data(), maximum.size()).size() == 65507, "maximum UDP size");
    maximum.push_back(8);
    must_throw([&] { ps::encode_udp(endpoint("192.0.2.1", 9), maximum.data(), maximum.size()); }, "oversized UDP");
    must_throw([&] { ps::encode_udp(endpoint("192.0.2.1", 0), nullptr, 0); }, "zero UDP destination port");
}

void test_fragmentation_and_coalescing() {
    MockServer server([](SOCKET socket, const std::atomic_bool& stop) {
        noauth(socket, stop, true);
        auto request = command(socket, stop);
        require(request == Bytes({5, 1, 0, 1, 198, 51, 100, 1, 1, 187}), "IPv4 CONNECT target preserved");
        write(socket, {5, 0, 0, 1, 127, 0, 0, 1, 0x12, 0x34, 'h', 'e', 'l', 'l', 'o'}, stop);
    });
    std::atomic_bool stop{false};
    auto socket = ps::socks_connect(server.proxy(), endpoint("198.51.100.1", 443), soon(), stop);
    require(read(socket.get(), 5, stop) == Bytes({'h', 'e', 'l', 'l', 'o'}), "coalesced application bytes preserved");
    server.finish();
}

void test_auth_and_ipv6() {
    MockServer server([](SOCKET socket, const std::atomic_bool& stop) {
        require(read(socket, 3, stop) == Bytes({5, 1, 2}), "credentials only offer userpass");
        write(socket, {5, 2}, stop, true);
        require(read(socket, 2, stop) == Bytes({1, 5}), "RFC1929 username length");
        require(read(socket, 5, stop) == Bytes({'a', 'l', 'i', 'c', 'e'}), "RFC1929 username");
        require(read(socket, 1, stop) == Bytes{6}, "RFC1929 password length");
        require(read(socket, 6, stop) == Bytes({'s', 'e', 'c', 'r', 'e', 't'}), "RFC1929 password");
        write(socket, {1, 0}, stop, true);
        auto request = command(socket, stop);
        require(request[3] == 4 && request.size() == 22 && request[4] == 0x20 && request[5] == 1 && request[20] == 1 && request[21] == 187, "IPv6 CONNECT target preserved");
        Bytes reply{5, 0, 0, 4, 0x20, 1, 0xd, 0xb8};
        reply.resize(20, 0); reply[19] = 9;
        reply.push_back(0x10); reply.push_back(0x92);
        write(socket, reply, stop, true);
    }, AF_INET6);
    std::atomic_bool stop{false};
    auto socket = ps::socks_authenticated(server.proxy(true), soon(), stop);
    auto bound = ps::socks_command(socket.get(), 1, endpoint("2001:db8::1", 443), soon(), stop);
    require(ps::same_endpoint(bound, endpoint("2001:db8::9", 4242)), "fragmented IPv6 reply");
    server.finish();
}

void test_auth_refusal() {
    std::atomic_bool stop{false};
    for (auto method : {0, 0xff}) {
        MockServer server([method](SOCKET socket, const std::atomic_bool& serverStop) {
            require(read(socket, 3, serverStop) == Bytes({5, 1, 2}), "auth refusal greeting");
            write(socket, {5, static_cast<std::uint8_t>(method)}, serverStop);
        });
        must_throw([&] { ps::socks_authenticated(server.proxy(true), soon(), stop); }, "auth downgrade/refusal");
        server.finish();
    }
    MockServer server([](SOCKET socket, const std::atomic_bool& serverStop) {
        read(socket, 3, serverStop); write(socket, {5, 2}, serverStop);
        read(socket, 14, serverStop); write(socket, {1, 1}, serverStop);
    });
    must_throw([&] { ps::socks_authenticated(server.proxy(true), soon(), stop); }, "bad password");
    server.finish();
    ps::ProxyConfig invalid; invalid.host = "127.0.0.1"; invalid.authenticate = true;
    invalid.username = std::string(256, 'x'); invalid.password = "x";
    must_throw([&] { ps::socks_authenticated(invalid, soon(), stop); }, "oversized credentials");
}

void test_localized_failures() {
    struct RestoreLanguage {
        ~RestoreLanguage() { ps::set_language(ps::Language::EnUS); }
    } restore;
    for (const auto language : {ps::Language::EnUS, ps::Language::ZhCN}) {
        ps::set_language(language);
        const auto expect_failure = [&](auto&& action, ps::Msg id) {
            try { action(); }
            catch (const std::exception& error) {
                const std::string message = error.what();
                require(message == ps::tr(id), "runtime failure uses selected language");
                require(message.find("alice") == std::string::npos && message.find("secret") == std::string::npos,
                        "runtime failure must not disclose credentials");
                return;
            }
            throw std::runtime_error("localized runtime failure was expected");
        };
        MockServer server([](SOCKET socket, const std::atomic_bool& serverStop) {
            read(socket, 3, serverStop); write(socket, {5, 2}, serverStop);
            read(socket, 14, serverStop); write(socket, {1, 1}, serverStop);
        });
        std::atomic_bool stop{false};
        expect_failure([&] { ps::socks_authenticated(server.proxy(true), soon(), stop); }, ps::Msg::RtSocksAuthentication);
        server.finish();
        expect_failure([] { ps::Endpoint{}.port(); }, ps::Msg::RtInvalidEndpoint);
        const Bytes fragment{0, 0, 1, 1};
        expect_failure([&] { ps::decode_udp(fragment.data(), fragment.size()); }, ps::Msg::RtSocksUdpFragment);
        stop = true;
        expect_failure([&] { ps::resolve_endpoints("localhost", 80, SOCK_STREAM, soon(), stop); }, ps::Msg::RtCancelled);
    }
}

void test_udp_associate_and_domain_reply() {
    MockServer server([](SOCKET socket, const std::atomic_bool& stop) {
        noauth(socket, stop);
        require(command(socket, stop, 3) == Bytes({5, 3, 0, 1, 0, 0, 0, 0, 0, 0}), "UDP ASSOCIATE request");
        // The relay differs from the proxy address; the client must use this reply.
        write(socket, {5, 0, 0, 1, 192, 0, 2, 88, 0x15, 0xb3}, stop, true);
    });
    std::atomic_bool stop{false};
    auto socket = ps::socks_authenticated(server.proxy(), soon(), stop);
    auto relay = ps::socks_command(socket.get(), 3, ps::Endpoint::any(AF_INET), soon(), stop);
    require(ps::same_endpoint(relay, endpoint("192.0.2.88", 5555)), "actual UDP relay preserved");
    server.finish();
    MockServer domain([](SOCKET client, const std::atomic_bool& serverStop) {
        noauth(client, serverStop); command(client, serverStop);
        const std::string name = "does-not-resolve.invalid";
        Bytes reply{5, 0, 0, 3, static_cast<std::uint8_t>(name.size())};
        reply.insert(reply.end(), name.begin(), name.end());
        reply.insert(reply.end(), {0x15, 0xb3, 42});
        write(client, reply, serverStop, true);
    });
    auto domainSocket = ps::socks_authenticated(domain.proxy(), soon(), stop);
    auto bound = ps::socks_command(domainSocket.get(), 1, endpoint("192.0.2.1", 80), soon(), stop);
    require(bound.port() == 5555 && bound.unspecified(), "CONNECT bind domain does not require resolution");
    require(read(domainSocket.get(), 1, stop) == Bytes{42}, "domain reply preserves application byte");
    domain.finish();
    MockServer udpDomain([](SOCKET client, const std::atomic_bool& serverStop) {
        noauth(client, serverStop); command(client, serverStop, 3);
        write(client, {5, 0, 0, 3, 9, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', 0x15, 0xb4}, serverStop, true);
    });
    auto udpControl = ps::socks_authenticated(udpDomain.proxy(), soon(), stop);
    auto domainRelay = ps::socks_command(udpControl.get(), 3, ps::Endpoint::any(AF_INET), soon(), stop);
    require(domainRelay.port() == 5556 && ps::bypass_address(domainRelay, false), "UDP relay domain must resolve");
    udpDomain.finish();
}

void test_malformed_replies() {
    const std::vector<Bytes> replies{
        {4, 0, 0, 1}, {5, 0, 1, 1}, {5, 7, 0, 1}, {5, 0, 0, 99},
        {5, 0, 0, 3, 0}, {5, 0, 0, 1, 127}, {5, 0, 0, 4, 1, 2},
        {5, 0, 0, 3, 1, 0, 0, 80}
    };
    std::atomic_bool stop{false};
    for (const auto& reply : replies) {
        MockServer server([reply](SOCKET socket, const std::atomic_bool& serverStop) {
            noauth(socket, serverStop); command(socket, serverStop); write(socket, reply, serverStop);
        });
        must_throw([&] { ps::socks_connect(server.proxy(), endpoint("198.51.100.1", 443), soon(), stop); }, "malformed command reply");
        server.finish();
    }
}

void test_timeout_and_cancellation() {
    MockServer server([](SOCKET socket, const std::atomic_bool& stop) {
        read(socket, 3, stop);
        // Wait for the client to close; the test's destructor can also cancel this wait.
        try { read(socket, 1, stop); } catch (const std::exception&) {}
    });
    std::atomic_bool stop{false};
    auto start = std::chrono::steady_clock::now();
    must_throw([&] { ps::socks_authenticated(server.proxy(), start + 100ms, stop); }, "handshake timeout");
    require(std::chrono::steady_clock::now() - start < 1s, "timeout remains bounded");
    server.finish();
    MockServer cancelled([](SOCKET socket, const std::atomic_bool& serverStop) {
        read(socket, 3, serverStop);
        try { read(socket, 1, serverStop); } catch (const std::exception&) {}
    });
    std::thread trigger([&] { std::this_thread::sleep_for(75ms); stop = true; });
    start = std::chrono::steady_clock::now();
    bool threw = false;
    try { ps::socks_authenticated(cancelled.proxy(), start + 5s, stop); } catch (const std::exception&) { threw = true; }
    trigger.join();
    require(threw && std::chrono::steady_clock::now() - start < 1s, "cancellation interrupts wait");
    cancelled.finish();
    must_throw([&] { ps::resolve_endpoints("localhost", 80, SOCK_STREAM, soon(), stop); }, "cancelled DNS");
    stop = false;
    must_throw([&] { ps::resolve_endpoints("localhost", 80, SOCK_STREAM, std::chrono::steady_clock::now(), stop); }, "expired DNS deadline");
}

void test_partial_io_and_raii() {
    const Bytes expected(512 * 1024, 0xa5);
    MockServer server([&](SOCKET socket, const std::atomic_bool& stop) {
        std::this_thread::sleep_for(25ms);
        auto actual = read(socket, expected.size(), stop);
        require(actual == expected, "large partial send preserved");
        write(socket, {42}, stop);
    });
    std::atomic_bool stop{false};
    auto original = ps::connect_endpoint(endpoint("::ffff:127.0.0.1", server.address().port()), soon(), stop);
    require(ps::socket_peer(original.get()).family() == AF_INET6, "mapped IPv6 connects to IPv4 listener");
    int small = 1024;
    require(setsockopt(original.get(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "set small send buffer");
    ps::Socket moved(std::move(original));
    require(!original && moved, "socket move owns handle exactly once");
    ps::send_all(moved.get(), expected.data(), expected.size(), soon(), stop);
    require(read(moved.get(), 1, stop) == Bytes{42}, "large send acknowledged");
    server.finish();
    moved.reset(); require(!moved, "socket reset invalidates handle");
}
}

int main() {
    ps::set_language(ps::Language::EnUS);
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) { std::cerr << "WSAStartup failed\n"; return 1; }
    const std::pair<const char*, void(*)()> tests[]{
        {"bypass boundaries and endpoints", test_bypass_and_endpoints},
        {"UDP framing and malformed packets", test_udp},
        {"fragmented greeting and coalesced payload", test_fragmentation_and_coalescing},
        {"userpass and bytewise IPv6 reply", test_auth_and_ipv6},
        {"auth refusal and downgrade prevention", test_auth_refusal},
        {"localized runtime failures and credential redaction", test_localized_failures},
        {"UDP relay and domain reply", test_udp_associate_and_domain_reply},
        {"malformed command replies", test_malformed_replies},
        {"timeouts and cancellation", test_timeout_and_cancellation},
        {"partial I/O and socket ownership", test_partial_io_and_raii}
    };
    int failures = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n'; }
    }
    WSACleanup();
    return failures ? 1 : 0;
}
