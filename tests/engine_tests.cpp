#include "l10n.hpp"
// Real Winsock/SOCKS traffic with an in-process NetFilter boundary. These tests
// never load a driver and deliberately do not emulate WFP packet interception.
#include "net.hpp"
#include "engine.hpp"
#include "socks.hpp"
#include "log.hpp"
#include <nfapi.h>
#include <array>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace ps;
using namespace std::chrono_literals;
const std::atomic_bool running{false};
std::mutex mockMutex;
std::condition_variable mockChanged;
NF_EventHandler* callbacks = nullptr;
std::map<ENDPOINT_ID, NF_TCP_CONN_INFO> completed;
std::map<ENDPOINT_ID, NF_UDP_CONN_INFO> udpInfo;
std::map<ENDPOINT_ID, bool> disabled;
std::size_t directSends = 0;
std::vector<std::string> errors;
struct Injected { ENDPOINT_ID id; Endpoint remote; std::string payload; unsigned long flags; std::vector<unsigned char> options; };
std::vector<Injected> injected;
DWORD initFlags = 0;
bool validRules = false;
std::thread::id callingThread;
bool completionInCallbackThread = false;

void require(bool good, const std::string& message) { if (!good) throw std::runtime_error(message); }
Deadline soon() { return std::chrono::steady_clock::now() + 5s; }
const sockaddr* address(const Endpoint& e) { return reinterpret_cast<const sockaddr*>(&e.address); }
const unsigned char* bytes(const Endpoint& e) { return reinterpret_cast<const unsigned char*>(&e.address); }
template<typename Predicate> void wait_for(Predicate predicate, const char* what) {
    std::unique_lock<std::mutex> lock(mockMutex);
    require(mockChanged.wait_for(lock, 5s, predicate), std::string("Timed out waiting for ") + what);
}
Socket listen_local(Endpoint& endpoint) {
    Socket socket(::socket(endpoint.family(), SOCK_STREAM, IPPROTO_TCP));
    require(bool(socket), "create listener");
    require(bind(socket.get(), address(endpoint), endpoint.length) == 0, "bind listener");
    require(listen(socket.get(), 8) == 0, "listen");
    nonblocking(socket.get()); endpoint = socket_local(socket.get());
    return socket;
}
Socket accept_one(SOCKET listener) {
    wait_socket(listener, false, soon(), running);
    Socket client(accept(listener, nullptr, nullptr));
    require(bool(client), "accept"); nonblocking(client.get()); return client;
}
void connect_bound(SOCKET socket, const Endpoint& target) {
    nonblocking(socket);
    const int result = connect(socket, address(target), target.length);
    require(result == 0 || WSAGetLastError() == WSAEWOULDBLOCK, "connect bound client");
    if (result) wait_socket(socket, true, soon(), running);
}
Endpoint global_target(int family = AF_INET) {
    return resolve_endpoints(family == AF_INET ? "198.51.100.17" : "2001:db8::17", 8080, SOCK_STREAM).front();
}
void greeting(SOCKET socket) {
    std::array<unsigned char, 3> request{};
    receive_exact(socket, request.data(), request.size(), soon(), running);
    require(request == std::array<unsigned char, 3>{5, 1, 0}, "greeting");
    const unsigned char response[]{5, 0};
    send_all(socket, response, sizeof(response), soon(), running);
}
void command(SOCKET socket, unsigned char expected) {
    std::array<unsigned char, 4> header{};
    receive_exact(socket, header.data(), header.size(), soon(), running);
    require(header[0] == 5 && header[1] == expected && header[2] == 0, "SOCKS command");
    std::array<unsigned char, 18> remainder{};
    require(header[3] == 1 || header[3] == 4, "SOCKS address family");
    receive_exact(socket, remainder.data(), header[3] == 1 ? 6 : 18, soon(), running);
}
void reply(SOCKET socket, const Endpoint& endpoint) {
    // UDP encoding and a success command reply share their address tail.
    auto data = encode_udp(endpoint, nullptr, 0);
    data[0] = 5; data[1] = 0;
    send_all(socket, data.data(), data.size(), soon(), running);
}
Config configuration(std::uint16_t port) {
    Config config;
    config.proxy.host = "127.0.0.1"; config.proxy.port = port;
    config.processes = {L"test.exe"};
    return config;
}
NF_TCP_CONN_INFO tcp_request(unsigned long pid, Endpoint local, Endpoint target) {
    NF_TCP_CONN_INFO result{};
    result.processId = pid; result.direction = NF_D_OUT; result.ip_family = static_cast<unsigned short>(target.family());
    std::memcpy(result.localAddress, &local.address, local.length);
    std::memcpy(result.remoteAddress, &target.address, target.length);
    return result;
}
void reset_mock() {
    std::lock_guard<std::mutex> lock(mockMutex);
    completed.clear(); udpInfo.clear(); disabled.clear(); directSends = 0;
    injected.clear(); errors.clear(); validRules = false;
    callingThread = std::this_thread::get_id(); completionInCallbackThread = false;
}

void tcp_relay_test(int family, bool mapped = false, bool wildcard = false) {
    reset_mock();
    auto serverAddress = Endpoint::loopback(AF_INET, 0);
    auto listener = listen_local(serverAddress);
    std::string payload(200000, 'x');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>('a' + i % 26);
    auto server = std::async(std::launch::async, [&] {
        auto control = accept_one(listener.get());
        greeting(control.get()); command(control.get(), 1);
        reply(control.get(), Endpoint::loopback(AF_INET, 42));
        std::string received(payload.size(), '\0');
        receive_exact(control.get(), received.data(), received.size(), soon(), running);
        require(received == payload, "TCP payload / partial sends");
        wait_socket(control.get(), false, soon(), running);
        char byte;
        require(recv(control.get(), &byte, 1, 0) == 0, "TCP half-close did not reach upstream");
        const char response[] = "response-after-FIN";
        send_all(control.get(), response, sizeof(response), soon(), running);
        shutdown(control.get(), SD_SEND);
    });
    auto config = configuration(serverAddress.port());
    ProcessTracker tracker(config); Engine engine(config, tracker); engine.start();
    require(validRules, "all-process TCP / UDP rules");
    require((initFlags & (NFF_DISABLE_AUTO_REGISTER | NFF_DISABLE_AUTO_START)) == (NFF_DISABLE_AUTO_REGISTER | NFF_DISABLE_AUTO_START), "implicit driver registration disabled");
    Socket client(::socket(family, SOCK_STREAM, IPPROTO_TCP));
    if (mapped) {
        DWORD only = 0;
        require(setsockopt(client.get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof(only)) == 0, "dual-stack application socket");
    }
    auto local = mapped ? resolve_endpoints(wildcard ? "::ffff:0.0.0.0" : "::ffff:127.0.0.1", 0, SOCK_STREAM).front() :
        (wildcard ? Endpoint::any(family, 0) : Endpoint::loopback(family, 0));
    require(bind(client.get(), address(local), local.length) == 0, "bind application client");
    local = socket_local(client.get());
    auto target = mapped ? resolve_endpoints("::ffff:198.51.100.17", 8080, SOCK_STREAM).front() : global_target(family);
    auto info = tcp_request(100, local, target);
    callbacks->tcpConnectRequest(11, &info);
    require(info.filteringFlag == NF_PEND_CONNECT_REQUEST, "TCP request was not pended");
    wait_for([] { return completed.count(11) != 0; }, "TCP completion");
    NF_TCP_CONN_INFO redirected;
    { std::lock_guard<std::mutex> lock(mockMutex); redirected = completed.at(11); }
    require(redirected.filteringFlag == NF_ALLOW, "TCP proxy unexpectedly blocked");
    require(!completionInCallbackThread, "TCP completed on SDK callback thread");
    auto relay = Endpoint::from_raw(redirected.remoteAddress);
    // A peer with the wrong source port must never be assigned this connection.
    auto attacker = connect_endpoint(relay, soon(), running);
    const char fake[] = "wrong-peer";
    send_all(attacker.get(), fake, sizeof(fake), soon(), running);
    connect_bound(client.get(), relay);
    send_all(client.get(), payload.data(), payload.size(), soon(), running);
    shutdown(client.get(), SD_SEND);
    std::array<char, sizeof("response-after-FIN")> response{};
    receive_exact(client.get(), response.data(), response.size(), soon(), running);
    require(std::string(response.data()) == "response-after-FIN", "TCP response after client FIN lost");
    server.get();
    callbacks->tcpClosed(11, &info);
    engine.stop();
}

void tcp_policy_test() {
    reset_mock();
    auto config = configuration(1); ProcessTracker tracker(config); Engine engine(config, tracker); engine.start();
    auto local = Endpoint::loopback(AF_INET, 50000);
    auto direct = tcp_request(101, local, global_target());
    callbacks->tcpConnectRequest(21, &direct);
    require(direct.filteringFlag == NF_ALLOW && same_endpoint(Endpoint::from_raw(direct.remoteAddress), global_target()), "non-selected TCP was redirected");
    auto uncachedDirect = tcp_request(103, local, global_target());
    callbacks->tcpConnectRequest(25, &uncachedDirect);
    wait_for([] { return completed.count(25) != 0; }, "uncached direct TCP");
    require(completed.at(25).filteringFlag == NF_ALLOW, "uncached non-selected TCP blocked");
    auto unknown = tcp_request(102, local, global_target());
    callbacks->tcpConnectRequest(22, &unknown);
    wait_for([] { return completed.count(22) != 0; }, "unknown TCP");
    require(completed.at(22).filteringFlag == NF_BLOCK, "unknown TCP did not fail closed");
    auto proxy = tcp_request(100, local, global_target());
    callbacks->tcpConnectRequest(23, &proxy);
    wait_for([] { return completed.count(23) != 0; }, "unavailable proxy TCP");
    require(completed.at(23).filteringFlag == NF_BLOCK, "unavailable TCP proxy leaked direct");
    auto bypass = tcp_request(100, local, Endpoint::loopback(AF_INET, 80));
    callbacks->tcpConnectRequest(24, &bypass);
    require(bypass.filteringFlag == NF_ALLOW && !completed.count(24), "loopback bypass");
    engine.stop();
}

void udp_relay_test(bool mappedRelay = false) {
    reset_mock();
    auto serverAddress = Endpoint::loopback(AF_INET, 0);
    auto listener = listen_local(serverAddress);
    // Deliberately different from the TCP control peer address.
    auto relayAddress = resolve_endpoints("127.0.0.2", 0, SOCK_DGRAM).front();
    Socket relay(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    require(bind(relay.get(), address(relayAddress), relayAddress.length) == 0, "bind alternate UDP relay");
    relayAddress = socket_local(relay.get()); nonblocking(relay.get());
    auto server = std::async(std::launch::async, [&] {
        auto control = accept_one(listener.get());
        greeting(control.get()); command(control.get(), 3);
        const auto advertised = mappedRelay ? resolve_endpoints("::ffff:127.0.0.2", relayAddress.port(), SOCK_DGRAM).front() : relayAddress;
        reply(control.get(), advertised);
        wait_socket(relay.get(), false, soon(), running);
        std::array<unsigned char, 2048> buffer{}; sockaddr_storage client{}; int clientLength = sizeof(client);
        const int count = recvfrom(relay.get(), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                   reinterpret_cast<sockaddr*>(&client), &clientLength);
        require(count > 0, "UDP relay did not receive encapsulation");
        const auto decoded = decode_udp(buffer.data(), count);
        require(same_endpoint(decoded.remote, global_target()), "UDP original destination lost");
        require(std::string(decoded.payload.begin(), decoded.payload.end()) == "udp-payload", "UDP payload changed");
        // A forged packet from a different UDP source must be rejected.
        Socket stranger(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        const auto wrong = encode_udp(global_target(), "forged", 6);
        sendto(stranger.get(), reinterpret_cast<const char*>(wrong.data()), static_cast<int>(wrong.size()), 0,
               reinterpret_cast<const sockaddr*>(&client), clientLength);
        const auto response = encode_udp(global_target(), "real-reply", 10);
        require(sendto(relay.get(), reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0,
                       reinterpret_cast<const sockaddr*>(&client), clientLength) == static_cast<int>(response.size()), "UDP relay reply");
        wait_for([] { return !injected.empty(); }, "UDP injection before control close");
        // The control connection's destruction terminates the association.
    });
    auto config = configuration(serverAddress.port()); ProcessTracker tracker(config); Engine engine(config, tracker); engine.start();
    NF_UDP_CONN_INFO info{}; info.processId = 100; info.ip_family = AF_INET;
    auto local = Endpoint::loopback(AF_INET, 50001);
    std::memcpy(info.localAddress, &local.address, local.length);
    { std::lock_guard<std::mutex> lock(mockMutex); udpInfo[31] = info; }
    callbacks->udpCreated(31, &info);
    struct Options { unsigned long flags; long length; unsigned char data[4]; } options{0x20, 4, {1, 2, 3, 4}};
    static_assert(offsetof(Options, data) == offsetof(NF_UDP_OPTIONS, options));
    const auto target = global_target();
    callbacks->udpSend(31, bytes(target), "udp-payload", 11, reinterpret_cast<PNF_UDP_OPTIONS>(&options));
    wait_for([] { return !injected.empty(); }, "UDP reply");
    {
        std::lock_guard<std::mutex> lock(mockMutex);
        require(injected.size() == 1 && injected[0].payload == "real-reply", "forged UDP accepted or reply lost");
        require(same_endpoint(injected[0].remote, target), "UDP source reconstruction");
        require(injected[0].options == std::vector<unsigned char>({1, 2, 3, 4}) && injected[0].flags == 0x20, "UDP ancillary metadata not retained");
        require(directSends == 0, "proxied UDP leaked to original target");
    }
    server.get();
    wait_for([] {
        for (const auto& e : errors) if (e.find("control channel closed") != std::string::npos) return true;
        return false;
    }, "UDP control closure");
    callbacks->udpSend(31, bytes(target), "later", 5, reinterpret_cast<PNF_UDP_OPTIONS>(&options));
    callbacks->udpClosed(31, &info);
    engine.stop();
    require(directSends == 0, "failed UDP association leaked direct");
}

void udp_direct_and_stop_test() {
    reset_mock();
    auto config = configuration(1); ProcessTracker tracker(config); Engine engine(config, tracker); engine.start();
    NF_UDP_CONN_INFO info{}; info.processId = 101; info.ip_family = AF_INET;
    const auto local = Endpoint::loopback(AF_INET, 50002), target = global_target();
    std::memcpy(info.localAddress, &local.address, local.length);
    NF_UDP_OPTIONS options{};
    callbacks->udpCreated(41, &info);
    callbacks->udpSend(41, bytes(target), "direct", 6, &options);
    wait_for([] { return directSends == 1; }, "direct UDP");
    callbacks->udpClosed(41, &info);
    info.processId = 100;
    callbacks->udpCreated(42, &info);
    callbacks->udpSend(42, bytes(target), "fail", 4, &options);
    // Stop while handshake may still be pending, then repeat stop to exercise
    // callback/worker ownership and idempotent shutdown without nfapi unload.
    engine.stop(); engine.stop();
    require(directSends == 1, "UDP failure/stop leaked direct");
}

void shutdown_race_test() {
    reset_mock();
    auto config = configuration(1); ProcessTracker tracker(config); Engine engine(config, tracker); engine.start();
    const auto local = Endpoint::loopback(AF_INET, 50003), target = global_target();
    for (ENDPOINT_ID id = 1000; id < 1064; ++id) {
        auto info = tcp_request(103, local, target); // Cache miss, immediately resolves Direct in worker.
        callbacks->tcpConnectRequest(id, &info);
        require(info.filteringFlag == NF_PEND_CONNECT_REQUEST, "racing request not pended");
    }
    NF_UDP_OPTIONS options{};
    for (ENDPOINT_ID id = 2000; id < 2032; ++id) {
        NF_UDP_CONN_INFO info{}; info.processId = 103; info.ip_family = AF_INET;
        std::memcpy(info.localAddress, &local.address, local.length);
        callbacks->udpCreated(id, &info);
        callbacks->udpSend(id, bytes(target), "race", 4, &options);
        callbacks->udpClosed(id, &info);
    }
    engine.stop();
    require(completed.size() == 64, "shutdown left pending TCP connect requests");
    require(!completionInCallbackThread, "completion reentered SDK callback thread");
}
}

namespace ps {
struct ProcessTracker::Impl {};
ProcessTracker::ProcessTracker(const Config&) : impl_(std::make_unique<Impl>()) {}
ProcessTracker::~ProcessTracker() = default;
void ProcessTracker::start() {}
void ProcessTracker::stop() {}
ProcessDecision ProcessTracker::classify(std::uint32_t pid, std::chrono::milliseconds timeout) {
    if (pid == 103) return timeout.count() == 0 ? ProcessDecision::Unknown : ProcessDecision::Direct;
    return pid == 100 ? ProcessDecision::Proxy : pid == 101 ? ProcessDecision::Direct : ProcessDecision::Unknown;
}
void log_info(const std::string&) {}
void log_debug(const std::string&) {}
void log_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(mockMutex); errors.push_back(message); mockChanged.notify_all();
}
void set_log_level(const std::string&) {}
}

extern "C" {
void NFAPI_CC nf_setOptions(DWORD, DWORD flags) { initFlags = flags; }
NF_STATUS NFAPI_CC nf_init(const char* name, NF_EventHandler* handler) {
    if (std::string(name) != "procsocks_nf") return NF_STATUS_FAIL;
    callbacks = handler; return NF_STATUS_SUCCESS;
}
void NFAPI_CC nf_free() { callbacks = nullptr; }
NF_STATUS NFAPI_CC nf_setRules(PNF_RULE rules, int count) {
    validRules = count == 3 && rules[0].processId == GetCurrentProcessId() && rules[0].filteringFlag == NF_ALLOW &&
        rules[1].protocol == IPPROTO_TCP && rules[1].processId == 0 && rules[1].filteringFlag == NF_INDICATE_CONNECT_REQUESTS &&
        rules[2].protocol == IPPROTO_UDP && rules[2].processId == 0 && rules[2].filteringFlag == NF_FILTER;
    return validRules ? NF_STATUS_SUCCESS : NF_STATUS_FAIL;
}
NF_STATUS NFAPI_CC nf_deleteRules() { return NF_STATUS_SUCCESS; }
NF_STATUS NFAPI_CC nf_completeTCPConnectRequest(ENDPOINT_ID id, PNF_TCP_CONN_INFO info) {
    std::lock_guard<std::mutex> lock(mockMutex);
    if (std::this_thread::get_id() == callingThread) completionInCallbackThread = true;
    completed[id] = *info; mockChanged.notify_all(); return NF_STATUS_SUCCESS;
}
NF_STATUS NFAPI_CC nf_tcpClose(ENDPOINT_ID) { return NF_STATUS_SUCCESS; }
NF_STATUS NFAPI_CC nf_tcpPostReceive(ENDPOINT_ID, const char*, int) { return NF_STATUS_SUCCESS; }
NF_STATUS NFAPI_CC nf_tcpPostSend(ENDPOINT_ID, const char*, int) { return NF_STATUS_SUCCESS; }
NF_STATUS NFAPI_CC nf_udpPostSend(ENDPOINT_ID id, const unsigned char*, const char*, int, PNF_UDP_OPTIONS options) {
    require(options != nullptr, "nf_udpPostSend requires metadata");
    { std::lock_guard<std::mutex> lock(mockMutex); ++directSends; mockChanged.notify_all(); }
    // Reentrant event deliberately probes that no engine map/queue locks are
    // held across the SDK boundary, regardless of the real SDK's dispatch mode.
    callbacks->udpCanReceive(id);
    return NF_STATUS_SUCCESS;
}
NF_STATUS NFAPI_CC nf_udpPostReceive(ENDPOINT_ID id, const unsigned char* remote, const char* data, int length, PNF_UDP_OPTIONS options) {
    require(options != nullptr, "nf_udpPostReceive requires metadata");
    {
        std::lock_guard<std::mutex> lock(mockMutex);
        injected.push_back({id, Endpoint::from_raw(remote), std::string(data, length), options->flags,
                           std::vector<unsigned char>(options->options, options->options + options->optionsLength)});
        mockChanged.notify_all();
    }
    callbacks->udpCanReceive(id); return NF_STATUS_SUCCESS;
}
NF_STATUS NFAPI_CC nf_udpDisableFiltering(ENDPOINT_ID id) {
    std::lock_guard<std::mutex> lock(mockMutex); disabled[id] = true; mockChanged.notify_all(); return NF_STATUS_SUCCESS;
}
NF_STATUS NFAPI_CC nf_getUDPConnInfo(ENDPOINT_ID id, PNF_UDP_CONN_INFO info) {
    std::lock_guard<std::mutex> lock(mockMutex);
    const auto found = udpInfo.find(id);
    if (found == udpInfo.end()) return NF_STATUS_INVALID_ENDPOINT_ID;
    *info = found->second; return NF_STATUS_SUCCESS;
}
}

int main() {
    ps::set_language(ps::Language::EnUS);
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return 1;
    try {
        tcp_relay_test(AF_INET);
        tcp_relay_test(AF_INET6);
        tcp_relay_test(AF_INET6, true);
        tcp_relay_test(AF_INET, false, true);
        tcp_relay_test(AF_INET6, false, true);
        tcp_relay_test(AF_INET6, true, true);
        tcp_policy_test();
        udp_relay_test();
        udp_relay_test(true);
        udp_direct_and_stop_test();
        shutdown_race_test();
        std::cout << "PASS engine: IPv4/IPv6/mapped TCP relay, large payload, half-close, peer validation, process decisions, fail-closed, UDP relay/metadata/control-close, shutdown\n";
        WSACleanup(); return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL engine: " << error.what() << '\n';
        for (const auto& message : errors) std::cerr << "  " << message << '\n';
        WSACleanup(); return 1;
    }
}
