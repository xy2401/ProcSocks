#include "net.hpp"
#include "l10n.hpp"
#include "engine.hpp"
#include "socks.hpp"
#include "log.hpp"
#ifndef _C_API
#define _C_API
#endif
#include <nfapi.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ps {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t MaxTcpSessions = 256;
constexpr std::size_t MaxUdpSessions = 128;
// Reserve classification workers independently of active proxy sessions, so
// a full proxy pool does not prevent new non-target processes being classified.
constexpr std::size_t MaxTcpWorkers = MaxTcpSessions + 64;
constexpr std::size_t MaxUdpWorkers = MaxUdpSessions + 32;
constexpr std::size_t MaxUdpEndpoints = 16384;
constexpr std::size_t MaxUdpPackets = 64;
constexpr std::size_t MaxUdpQueueBytes = 256 * 1024;
constexpr std::size_t MaxUdpDestinations = 256;
constexpr auto HandshakeTimeout = 10s;
constexpr auto TcpIdleTimeout = 30min;

void report(const std::string& message) noexcept {
    try { log_error(message); } catch (...) {}
}
void socket_check(bool good, Msg operation) {
    if (!good) {
        const auto error = WSAGetLastError();
        throw std::runtime_error(tr(Msg::RtEngineWinsock, {tr(operation), std::to_string(error)}));
    }
}
const sockaddr* addr(const Endpoint& e) { return reinterpret_cast<const sockaddr*>(&e.address); }
const unsigned char* raw(const Endpoint& e) { return reinterpret_cast<const unsigned char*>(&e.address); }

// A fixed-size buffer in each direction provides TCP backpressure. EOF is
// forwarded only after all preceding bytes have drained (independent FINs).
void relay_tcp(SOCKET client, SOCKET upstream, const std::atomic_bool& cancelled) {
    struct Direction {
        SOCKET source, destination;
        std::array<char, 65536> bytes{};
        std::size_t begin = 0, end = 0;
        bool eof = false, fin = false;
    };
    Direction toProxy{client, upstream}, toClient{upstream, client};
    auto lastActivity = Clock::now();
    while (!cancelled.load()) {
        fd_set reads, writes;
        FD_ZERO(&reads); FD_ZERO(&writes);
        for (auto* d : {&toProxy, &toClient}) {
            if (d->begin == d->end) d->begin = d->end = 0;
            if (d->begin && d->end == d->bytes.size()) {
                std::memmove(d->bytes.data(), d->bytes.data() + d->begin, d->end - d->begin);
                d->end -= d->begin; d->begin = 0;
            }
            if (d->eof && !d->fin && d->begin == d->end) {
                shutdown(d->destination, SD_SEND);
                d->fin = true;
            }
            if (!d->eof && d->end < d->bytes.size()) FD_SET(d->source, &reads);
            if (d->begin < d->end) FD_SET(d->destination, &writes);
        }
        if (toProxy.fin && toClient.fin) return;
        if (Clock::now() - lastActivity > TcpIdleTimeout) throw std::runtime_error(tr(Msg::RtTcpIdle));
        timeval timeout{0, 50000};
        const int ready = select(0, &reads, &writes, nullptr, &timeout);
        socket_check(ready != SOCKET_ERROR, Msg::RtTcpSelect);
        for (auto* d : {&toProxy, &toClient}) {
            if (FD_ISSET(d->destination, &writes)) {
                const int n = send(d->destination, d->bytes.data() + d->begin, static_cast<int>(d->end - d->begin), 0);
                if (n > 0) { d->begin += n; lastActivity = Clock::now(); }
                else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) socket_check(false, Msg::RtTcpSend);
            }
            if (FD_ISSET(d->source, &reads)) {
                const int n = recv(d->source, d->bytes.data() + d->end, static_cast<int>(d->bytes.size() - d->end), 0);
                if (n > 0) { d->end += n; lastActivity = Clock::now(); }
                else if (n == 0) d->eof = true;
                else if (WSAGetLastError() != WSAEWOULDBLOCK) socket_check(false, Msg::RtTcpReceive);
            }
        }
    }
}

Socket make_listener(Endpoint& bindAddress) {
    Socket socket(::socket(bindAddress.family(), SOCK_STREAM, IPPROTO_TCP));
    socket_check(bool(socket), Msg::RtTcpSocket);
    BOOL exclusive = TRUE;
    socket_check(setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                           reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0, Msg::RtTcpExclusiveBind);
    if (bindAddress.family() == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&bindAddress.address);
        DWORD v6Only = IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr) ? 0 : 1;
        socket_check(setsockopt(socket.get(), IPPROTO_IPV6, IPV6_V6ONLY,
                               reinterpret_cast<const char*>(&v6Only), sizeof(v6Only)) == 0, Msg::RtTcpIpv6Bind);
    }
    bindAddress.set_port(0);
    socket_check(bind(socket.get(), addr(bindAddress), bindAddress.length) == 0, Msg::RtTcpBind);
    socket_check(listen(socket.get(), 8) == 0, Msg::RtTcpListen);
    nonblocking(socket.get());
    bindAddress = socket_local(socket.get());
    return socket;
}

std::vector<unsigned char> copy_options(PNF_UDP_OPTIONS options) {
    if (!options) throw std::runtime_error(tr(Msg::RtUdpNoAncillary));
    if (options->optionsLength < 0 || options->optionsLength > 65536) throw std::runtime_error(tr(Msg::RtUdpAncillaryLength));
    const auto n = offsetof(NF_UDP_OPTIONS, options) + std::max<std::size_t>(1, options->optionsLength);
    std::vector<unsigned char> result(n, 0);
    // For optionsLength==0, there need not be a trailing byte to read.
    std::memcpy(result.data(), options, offsetof(NF_UDP_OPTIONS, options) + options->optionsLength);
    return result;
}
PNF_UDP_OPTIONS options_ptr(std::vector<unsigned char>& options) {
    return options.empty() ? nullptr : reinterpret_cast<PNF_UDP_OPTIONS>(options.data());
}

// SOCKS relays can return IPv4 while an application used IPv4-mapped IPv6.
// Compare a canonical key but reinject the application's original sockaddr.
Endpoint canonical(Endpoint value) {
    if (value.family() == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&value.address);
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            sockaddr_in in4{};
            in4.sin_family = AF_INET;
            in4.sin_port = in6->sin6_port;
            std::memcpy(&in4.sin_addr, in6->sin6_addr.s6_addr + 12, 4);
            return Endpoint::from_raw(&in4);
        }
    }
    return value;
}
std::string endpoint_key(const Endpoint& e) { return canonical(e).text(); }
Endpoint redirect_bind_address(const Endpoint& local) {
    const auto normalized = canonical(local);
    if (!normalized.unspecified()) return local;
    auto loopback = Endpoint::loopback(local.family(), 0);
    if (normalized.family() != local.family()) {
        // A socket bound to ::ffff:0.0.0.0 needs a mapped IPv4 loopback
        // redirect, rather than ::1 (which is incompatible with that bind).
        auto* value = reinterpret_cast<sockaddr_in6*>(&loopback.address);
        std::memset(&value->sin6_addr, 0, sizeof(value->sin6_addr));
        value->sin6_addr.s6_addr[10] = value->sin6_addr.s6_addr[11] = 255;
        value->sin6_addr.s6_addr[12] = 127; value->sin6_addr.s6_addr[15] = 1;
    }
    return loopback;
}
}

struct Engine::Impl {
    struct TcpSession {
        ENDPOINT_ID id = 0;
        NF_TCP_CONN_INFO info{};
        std::atomic_bool cancel{false}, done{false};
        std::thread thread;
    };
    struct Datagram {
        Endpoint remote;
        std::vector<char> data;
        std::vector<unsigned char> options;
        std::size_t bytes() const { return data.size() + options.size(); }
    };
    struct UdpSession {
        ENDPOINT_ID id = 0;
        NF_UDP_CONN_INFO info{};
        std::atomic_bool cancel{false}, done{false}, injectReady{true};
        std::thread thread;
        std::mutex mutex;
        std::deque<Datagram> queue;
        std::size_t queueBytes = 0;
        ProcessDecision decision = ProcessDecision::Unknown;
        bool started = false, failed = false, closed = false, overflowLogged = false;
    };
    Config config;
    ProcessTracker& tracker;
    std::atomic_bool stopping{false};
    bool initialized = false;
    NF_EventHandler handlers{};
    std::mutex mutex;
    std::condition_variable wake;
    std::map<ENDPOINT_ID, std::shared_ptr<TcpSession>> tcp;
    std::map<ENDPOINT_ID, std::shared_ptr<UdpSession>> udp;
    std::size_t udpWorkers = 0;
    std::size_t tcpProxies = 0, udpProxies = 0;
    std::thread reaper;
    static std::atomic<Impl*> active;

    Impl(const Config& c, ProcessTracker& t) : config(c), tracker(t) {}

    void complete_tcp(const std::shared_ptr<TcpSession>& s, NF_TCP_CONN_INFO& info, bool& completed) {
        const auto status = nf_completeTCPConnectRequest(s->id, &info);
        completed = true; // Never resubmit a possibly consumed request.
        if (status != NF_STATUS_SUCCESS) {
            nf_tcpClose(s->id);
            throw std::runtime_error(tr(Msg::RtTcpComplete, {std::to_string(status)}));
        }
    }

    void tcp_worker(const std::shared_ptr<TcpSession>& s) noexcept {
        bool completed = false;
        bool proxySlot = false;
        try {
            // The creator holds mutex until it has installed the session and
            // NF_PEND_CONNECT_REQUEST. Never race completion against those writes.
            { std::lock_guard<std::mutex> ready(mutex); }
            auto info = s->info;
            const auto decision = tracker.classify(info.processId, 1500ms);
            if (stopping.load() || s->cancel.load() || decision == ProcessDecision::Unknown) {
                info.filteringFlag = NF_BLOCK;
                complete_tcp(s, info, completed);
                if (decision == ProcessDecision::Unknown) report(tr(Msg::RtTcpClassificationTimeout, {std::to_string(s->info.processId)}));
            } else if (decision == ProcessDecision::Direct) {
                info.filteringFlag = NF_ALLOW;
                complete_tcp(s, info, completed);
            } else {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (tcpProxies >= MaxTcpSessions) throw std::runtime_error(tr(Msg::RtTcpCapacity));
                    ++tcpProxies; proxySlot = true;
                }
                const auto target = Endpoint::from_raw(info.remoteAddress);
                const auto originalLocal = Endpoint::from_raw(info.localAddress);
                if (!originalLocal.port()) throw std::runtime_error(tr(Msg::RtTcpNoLocalPort));
                Endpoint bindAddress = redirect_bind_address(originalLocal);
                auto listener = make_listener(bindAddress);
                // Establish the upstream before allowing the application connect.
                auto upstream = socks_connect(config.proxy, canonical(target), Clock::now() + HandshakeTimeout, s->cancel);
                if (s->cancel.load() || stopping.load()) throw std::runtime_error(tr(Msg::RtConnectionCancelled));
                std::memset(info.remoteAddress, 0, sizeof(info.remoteAddress));
                std::memcpy(info.remoteAddress, &bindAddress.address, bindAddress.length);
                info.processId = GetCurrentProcessId(); // SDK local redirect protection.
                info.filteringFlag = NF_ALLOW;
                complete_tcp(s, info, completed);
                Endpoint expectedPeer = canonical(originalLocal).unspecified() ? bindAddress : originalLocal;
                expectedPeer.set_port(originalLocal.port());
                const auto deadline = Clock::now() + 5s;
                Socket client;
                while (!client && !s->cancel.load()) {
                    wait_socket(listener.get(), false, deadline, s->cancel);
                    sockaddr_storage peer{};
                    int peerLength = sizeof(peer);
                    Socket candidate(accept(listener.get(), reinterpret_cast<sockaddr*>(&peer), &peerLength));
                    if (!candidate) {
                        if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
                        socket_check(false, Msg::RtTcpAccept);
                    }
                    if (!same_endpoint(Endpoint::from_raw(&peer), expectedPeer)) continue;
                    client = std::move(candidate);
                }
                listener.reset();
                if (!client) throw std::runtime_error(tr(Msg::RtTcpAcceptCancelled));
                nonblocking(client.get());
                log_debug(tr(Msg::RtTcpProxy, {std::to_string(s->info.processId), target.text()}));
                relay_tcp(client.get(), upstream.get(), s->cancel);
            }
        } catch (const std::exception& error) {
            if (!s->cancel.load() && !stopping.load()) report(tr(Msg::RtTcpBlocked, {std::to_string(s->info.processId), error.what()}));
        } catch (...) {
            report(tr(Msg::RtTcpWorkerFailed));
        }
        if (!completed) {
            auto info = s->info;
            info.filteringFlag = NF_BLOCK;
            nf_completeTCPConnectRequest(s->id, &info);
        }
        if (proxySlot) { std::lock_guard<std::mutex> lock(mutex); --tcpProxies; }
        s->done.store(true);
        wake.notify_one();
    }

    void on_tcp_connect(ENDPOINT_ID id, PNF_TCP_CONN_INFO info) {
        if (stopping.load()) { info->filteringFlag = NF_BLOCK; return; }
        if (info->processId == GetCurrentProcessId() ||
            bypass_address(Endpoint::from_raw(info->remoteAddress), config.bypassPrivateNetworks)) {
            info->filteringFlag = NF_ALLOW;
            return;
        }
        // Timeout zero is a cached lookup with PID-instance validation; it
        // neither waits for ETW nor takes a fresh whole-system snapshot.
        const auto cached = tracker.classify(info->processId, 0ms);
        if (cached == ProcessDecision::Direct) { info->filteringFlag = NF_ALLOW; return; }
        auto session = std::make_shared<TcpSession>();
        session->id = id; session->info = *info;
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping.load() || tcp.size() >= MaxTcpWorkers || tcp.count(id) ||
            (cached == ProcessDecision::Proxy && tcpProxies >= MaxTcpSessions)) {
            info->filteringFlag = NF_BLOCK;
            report(tr(Msg::RtTcpCapacityOrDuplicate));
            return;
        }
        tcp.emplace(id, session);
        info->filteringFlag = NF_PEND_CONNECT_REQUEST;
        try { session->thread = std::thread([this, session] { tcp_worker(session); }); }
        catch (...) { tcp.erase(id); info->filteringFlag = NF_BLOCK; throw; }
    }

    void on_tcp_closed(ENDPOINT_ID id) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = tcp.find(id);
        if (it != tcp.end()) it->second->cancel.store(true);
    }

    void on_udp_created(ENDPOINT_ID id, PNF_UDP_CONN_INFO info) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping.load() || udp.size() >= MaxUdpEndpoints || udp.count(id)) return;
        auto session = std::make_shared<UdpSession>();
        session->id = id; session->info = *info;
        udp.emplace(id, std::move(session));
    }

    std::shared_ptr<UdpSession> find_udp(ENDPOINT_ID id) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = udp.find(id);
        return it == udp.end() ? nullptr : it->second;
    }

    void on_udp_closed(ENDPOINT_ID id) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = udp.find(id);
        if (it == udp.end()) return;
        auto s = it->second;
        s->cancel.store(true);
        std::lock_guard<std::mutex> queueLock(s->mutex);
        s->closed = true;
        s->queue.clear(); s->queueBytes = 0;
        if (!s->thread.joinable()) udp.erase(it);
        wake.notify_one();
    }

    void on_udp_send(ENDPOINT_ID id, const unsigned char* remoteAddress, const char* data, int length, PNF_UDP_OPTIONS options) {
        if (stopping.load() || !options || length < 0 || length > 65507 || (length && !data)) return;
        const auto target = Endpoint::from_raw(remoteAddress);
        if (bypass_address(target, config.bypassPrivateNetworks)) {
            nf_udpPostSend(id, remoteAddress, data, length, options);
            return;
        }
        auto s = find_udp(id);
        if (!s) {
            NF_UDP_CONN_INFO info{};
            if (nf_getUDPConnInfo(id, &info) != NF_STATUS_SUCCESS) return;
            if (info.processId == GetCurrentProcessId() || tracker.classify(info.processId, 0ms) == ProcessDecision::Direct) {
                nf_udpPostSend(id, remoteAddress, data, length, options);
                return;
            }
            on_udp_created(id, &info);
            s = find_udp(id);
            if (!s) { report(tr(Msg::RtUdpEndpointCapacity)); return; }
        }
        const auto cached = tracker.classify(s->info.processId, 0ms);
        Datagram packet;
        packet.remote = target;
        if (length) packet.data.assign(data, data + length);
        packet.options = copy_options(options);
        // Global-before-session lock order is shared with shutdown/reaping.
        std::unique_lock<std::mutex> lock(mutex);
        std::unique_lock<std::mutex> queueLock(s->mutex);
        if (stopping.load() || s->closed || s->cancel.load() || s->failed) return;
        if (s->decision == ProcessDecision::Direct || s->info.processId == GetCurrentProcessId() ||
            (cached == ProcessDecision::Direct && !s->started)) {
            s->decision = ProcessDecision::Direct;
            queueLock.unlock(); lock.unlock();
            nf_udpPostSend(id, remoteAddress, data, length, options);
            return;
        }
        if (s->queue.size() >= MaxUdpPackets || s->queueBytes + packet.bytes() > MaxUdpQueueBytes) {
            if (!s->overflowLogged) { report(tr(Msg::RtUdpQueueFull, {std::to_string(id)})); s->overflowLogged = true; }
            return;
        }
        s->queueBytes += packet.bytes();
        s->queue.push_back(std::move(packet));
        if (!s->started) {
            if (udpWorkers >= MaxUdpWorkers || (cached == ProcessDecision::Proxy && udpProxies >= MaxUdpSessions)) {
                s->queue.clear(); s->queueBytes = 0;
                report(tr(Msg::RtUdpAssociationCapacity));
                return;
            }
            s->started = true;
            ++udpWorkers;
            try { s->thread = std::thread([this, s] { udp_worker(s); }); }
            catch (...) {
                --udpWorkers; s->failed = true; s->done.store(true);
                s->queue.clear(); s->queueBytes = 0;
                throw;
            }
        }
    }

    bool take_packet(const std::shared_ptr<UdpSession>& s, Datagram& packet) {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->queue.empty()) return false;
        packet = std::move(s->queue.front());
        s->queue.pop_front();
        s->queueBytes -= packet.bytes();
        if (s->queue.empty()) s->overflowLogged = false;
        return true;
    }

    void udp_worker(const std::shared_ptr<UdpSession>& s) noexcept {
        bool proxySlot = false;
        try {
            const auto decision = tracker.classify(s->info.processId, 1500ms);
            if (decision == ProcessDecision::Unknown) throw std::runtime_error(tr(Msg::RtProcessTimeout));
            if (s->cancel.load() || stopping.load()) throw std::runtime_error(tr(Msg::RtUdpCancelled));
            if (decision == ProcessDecision::Direct) {
                // Publish Direct only once all previously queued packets are
                // injected, preventing newer callbacks overtaking the queue.
                for (;;) {
                    if (s->cancel.load() || stopping.load()) throw std::runtime_error(tr(Msg::RtUdpCancelled));
                    Datagram packet;
                    {
                        std::lock_guard<std::mutex> lock(s->mutex);
                        if (s->queue.empty()) {
                            s->decision = decision;
                            break;
                        }
                        packet = std::move(s->queue.front()); s->queue.pop_front();
                        s->queueBytes -= packet.bytes();
                    }
                    const auto status = nf_udpPostSend(s->id, raw(packet.remote), packet.data.empty() ? "" : packet.data.data(),
                                                       static_cast<int>(packet.data.size()), options_ptr(packet.options));
                    if (status != NF_STATUS_SUCCESS) throw std::runtime_error(tr(Msg::RtUdpDirectInjection, {std::to_string(status)}));
                }
                // Keep lifecycle callbacks enabled: disabling all UDP events
                // can suppress udpClosed and strand per-endpoint metadata.
            } else {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (udpProxies >= MaxUdpSessions) throw std::runtime_error(tr(Msg::RtUdpCapacity));
                    ++udpProxies; proxySlot = true;
                }
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    s->decision = decision;
                }
                const auto deadline = Clock::now() + HandshakeTimeout;
                auto control = socks_authenticated(config.proxy, deadline, s->cancel);
                const auto peer = socket_peer(control.get());
                auto relay = canonical(socks_command(control.get(), 3, Endpoint::any(peer.family()), deadline, s->cancel));
                if (!relay.port()) throw std::runtime_error(tr(Msg::RtUdpRelayZeroPort));
                if (relay.unspecified()) { const auto port = relay.port(); relay = canonical(peer); relay.set_port(port); }
                Socket socket(::socket(relay.family(), SOCK_DGRAM, IPPROTO_UDP));
                socket_check(bool(socket), Msg::RtUdpSocket);
                const auto local = Endpoint::any(relay.family());
                socket_check(bind(socket.get(), addr(local), local.length) == 0, Msg::RtUdpBind);
                // A connected UDP socket also asks the kernel to reject packets
                // from any source other than the authenticated SOCKS relay.
                socket_check(connect(socket.get(), addr(relay), relay.length) == 0, Msg::RtUdpRelayConnect);
                nonblocking(socket.get());
                log_debug(tr(Msg::RtUdpProxy, {std::to_string(s->info.processId), relay.text()}));
                std::map<std::string, Datagram> destinations;
                std::size_t metadataBytes = 0;
                std::array<unsigned char, 65536> incoming{};
                while (!s->cancel.load()) {
                    for (int batch = 0; batch < 16; ++batch) {
                        Datagram packet;
                        if (!take_packet(s, packet)) break;
                        const auto key = endpoint_key(packet.remote);
                        if (!destinations.count(key) && destinations.size() >= MaxUdpDestinations) {
                            // Bounded route/options table: reject new destinations,
                            // retaining valid reply metadata for existing ones.
                            report(tr(Msg::RtUdpDestinationLimit));
                            continue;
                        }
                        const auto previous = destinations.find(key);
                        const auto previousBytes = previous == destinations.end() ? 0 : previous->second.options.size();
                        if (metadataBytes - previousBytes + packet.options.size() > MaxUdpQueueBytes) {
                            report(tr(Msg::RtUdpMetadataLimit));
                            continue;
                        }
                        const auto wireRemote = canonical(packet.remote);
                        const std::size_t headerBytes = wireRemote.family() == AF_INET ? 10 : 22;
                        if (packet.data.size() > 65507 - headerBytes) { report(tr(Msg::RtUdpEncapsulationLimit)); continue; }
                        const auto encoded = encode_udp(wireRemote, packet.data.data(), packet.data.size());
                        const auto sendDeadline = Clock::now() + 3s;
                        for (;;) {
                            const int sent = send(socket.get(), reinterpret_cast<const char*>(encoded.data()), static_cast<int>(encoded.size()), 0);
                            if (sent == static_cast<int>(encoded.size())) break;
                            if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                                wait_socket(socket.get(), true, sendDeadline, s->cancel);
                                continue;
                            }
                            socket_check(false, Msg::RtUdpSend);
                        }
                        packet.data.clear(); packet.data.shrink_to_fit();
                        metadataBytes = metadataBytes - previousBytes + packet.options.size();
                        destinations.insert_or_assign(key, std::move(packet));
                    }
                    fd_set reads;
                    FD_ZERO(&reads); FD_SET(control.get(), &reads);
                    if (s->injectReady.load()) FD_SET(socket.get(), &reads);
                    timeval timeout{0, 50000};
                    socket_check(select(0, &reads, nullptr, nullptr, &timeout) != SOCKET_ERROR, Msg::RtUdpSelect);
                    if (FD_ISSET(control.get(), &reads)) {
                        char byte;
                        const int n = recv(control.get(), &byte, 1, MSG_PEEK);
                        if (n == 0) throw std::runtime_error(tr(Msg::RtUdpControlClosed));
                        if (n > 0) throw std::runtime_error(tr(Msg::RtUdpControlUnexpected));
                        if (WSAGetLastError() != WSAEWOULDBLOCK) socket_check(false, Msg::RtUdpControlReceive);
                    }
                    if (FD_ISSET(socket.get(), &reads)) {
                        sockaddr_storage source{}; int sourceLength = sizeof(source);
                        const int n = recvfrom(socket.get(), reinterpret_cast<char*>(incoming.data()), static_cast<int>(incoming.size()), 0,
                                               reinterpret_cast<sockaddr*>(&source), &sourceLength);
                        if (n == SOCKET_ERROR) {
                            const int error = WSAGetLastError();
                            if (error == WSAEWOULDBLOCK || error == WSAEMSGSIZE || error == WSAECONNRESET) continue;
                            socket_check(false, Msg::RtUdpReceive);
                        }
                        if (!same_endpoint(Endpoint::from_raw(&source), relay)) continue;
                        UdpPacket decoded;
                        try { decoded = decode_udp(incoming.data(), n); }
                        catch (const std::exception& e) { report(tr(Msg::RtUdpMalformed, {e.what()})); continue; }
                        const auto found = destinations.find(endpoint_key(decoded.remote));
                        if (found == destinations.end()) continue;
                        s->injectReady.store(false); // bounded SDK receive buffering
                        auto& original = found->second;
                        const auto status = nf_udpPostReceive(s->id, raw(original.remote), decoded.payload.empty() ? "" : reinterpret_cast<const char*>(decoded.payload.data()),
                                                              static_cast<int>(decoded.payload.size()), options_ptr(original.options));
                        if (status != NF_STATUS_SUCCESS) {
                            s->injectReady.store(true);
                            if (status == NF_STATUS_INVALID_ENDPOINT_ID) s->cancel.store(true);
                            else throw std::runtime_error(tr(Msg::RtUdpInjection, {std::to_string(status)}));
                        }
                    }
                }
            }
        } catch (const std::exception& error) {
            if (!s->cancel.load() && !stopping.load()) report(tr(Msg::RtUdpBlocked, {std::to_string(s->info.processId), error.what()}));
            std::lock_guard<std::mutex> lock(s->mutex);
            s->failed = true; s->queue.clear(); s->queueBytes = 0;
        } catch (...) {
            report(tr(Msg::RtUdpWorkerFailed));
            std::lock_guard<std::mutex> lock(s->mutex);
            s->failed = true; s->queue.clear(); s->queueBytes = 0;
        }
        if (proxySlot) { std::lock_guard<std::mutex> lock(mutex); --udpProxies; }
        s->done.store(true);
        wake.notify_one();
    }

    void reap() noexcept {
        // Never join while holding the map/queue mutex: callbacks and workers
        // are allowed to finish concurrently, and callback memory stays alive.
        try {
            while (!stopping.load()) {
                std::array<std::thread, MaxTcpWorkers + MaxUdpWorkers> finished;
                std::size_t finishedCount = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, 50ms);
                    for (auto it = tcp.begin(); it != tcp.end();) {
                        if (it->second->done.load()) {
                            if (it->second->thread.joinable()) finished[finishedCount++] = std::move(it->second->thread);
                            it = tcp.erase(it);
                        } else ++it;
                    }
                    for (auto it = udp.begin(); it != udp.end();) {
                        const auto s = it->second;
                        if (s->done.load() && s->thread.joinable()) {
                            finished[finishedCount++] = std::move(s->thread);
                            --udpWorkers;
                        }
                        std::lock_guard<std::mutex> queueLock(s->mutex);
                        if (s->closed && !s->thread.joinable()) it = udp.erase(it);
                        else ++it;
                    }
                }
                for (std::size_t i = 0; i < finishedCount; ++i) finished[i].join();
            }
        } catch (...) { report(tr(Msg::RtReaperFailed)); }
    }

    template<typename Function>
    static void guarded(Function&& fn) noexcept {
        try { if (auto* self = active.load()) fn(*self); }
        catch (const std::exception& e) { report(tr(Msg::RtCallbackFailedDetail, {e.what()})); }
        catch (...) { report(tr(Msg::RtCallbackFailed)); }
    }
    static void NFAPI_CC thread_event() noexcept {}
    static void NFAPI_CC tcp_connect(ENDPOINT_ID id, PNF_TCP_CONN_INFO info) noexcept {
        info->filteringFlag = NF_BLOCK;
        guarded([&](Impl& self) { self.on_tcp_connect(id, info); });
    }
    static void NFAPI_CC tcp_connected(ENDPOINT_ID, PNF_TCP_CONN_INFO) noexcept {}
    static void NFAPI_CC tcp_closed(ENDPOINT_ID id, PNF_TCP_CONN_INFO) noexcept { guarded([&](Impl& self) { self.on_tcp_closed(id); }); }
    static void NFAPI_CC tcp_receive(ENDPOINT_ID id, const char* data, int length) noexcept { nf_tcpPostReceive(id, data, length); }
    static void NFAPI_CC tcp_send(ENDPOINT_ID id, const char* data, int length) noexcept { nf_tcpPostSend(id, data, length); }
    static void NFAPI_CC tcp_ready(ENDPOINT_ID) noexcept {}
    static void NFAPI_CC udp_created(ENDPOINT_ID id, PNF_UDP_CONN_INFO info) noexcept { guarded([&](Impl& self) { self.on_udp_created(id, info); }); }
    static void NFAPI_CC udp_connect(ENDPOINT_ID, PNF_UDP_CONN_REQUEST info) noexcept {
        info->filteringFlag = NF_FILTER; // Keep the original peer; redirect only encapsulated datagrams.
    }
    static void NFAPI_CC udp_closed(ENDPOINT_ID id, PNF_UDP_CONN_INFO) noexcept { guarded([&](Impl& self) { self.on_udp_closed(id); }); }
    static void NFAPI_CC udp_receive(ENDPOINT_ID id, const unsigned char* remote, const char* data, int length, PNF_UDP_OPTIONS options) noexcept {
        // These are native replies (including LAN bypass), not SOCKS responses.
        if (options && length >= 0 && (length == 0 || data)) nf_udpPostReceive(id, remote, data, length, options);
    }
    static void NFAPI_CC udp_send(ENDPOINT_ID id, const unsigned char* remote, const char* data, int length, PNF_UDP_OPTIONS options) noexcept {
        guarded([&](Impl& self) { self.on_udp_send(id, remote, data, length, options); });
    }
    static void NFAPI_CC udp_can_receive(ENDPOINT_ID id) noexcept {
        guarded([&](Impl& self) { const auto s = self.find_udp(id); if (s) s->injectReady.store(true); });
    }
    static void NFAPI_CC udp_can_send(ENDPOINT_ID) noexcept {}

    void start() {
        if (initialized) throw std::runtime_error(tr(Msg::RtEngineStarted));
        Impl* expected = nullptr;
        if (!active.compare_exchange_strong(expected, this)) throw std::runtime_error(tr(Msg::RtEngineSingle));
        stopping.store(false);
        handlers.threadStart = thread_event; handlers.threadEnd = thread_event;
        handlers.tcpConnectRequest = tcp_connect; handlers.tcpConnected = tcp_connected; handlers.tcpClosed = tcp_closed;
        handlers.tcpReceive = tcp_receive; handlers.tcpSend = tcp_send; handlers.tcpCanReceive = tcp_ready; handlers.tcpCanSend = tcp_ready;
        handlers.udpCreated = udp_created; handlers.udpConnectRequest = udp_connect; handlers.udpClosed = udp_closed;
        handlers.udpReceive = udp_receive; handlers.udpSend = udp_send; handlers.udpCanReceive = udp_can_receive; handlers.udpCanSend = udp_can_send;
        nf_setOptions(1, NFF_DISABLE_AUTO_REGISTER | NFF_DISABLE_AUTO_START | NFF_DONT_DISABLE_TEREDO | NFF_DONT_DISABLE_TCP_OFFLOADING);
        const auto status = nf_init(DriverName, &handlers);
        if (status != NF_STATUS_SUCCESS) {
            active.store(nullptr);
            throw std::runtime_error(tr(Msg::RtNfInitialization, {std::to_string(status)}));
        }
        initialized = true;
        try {
            reaper = std::thread([this] { reap(); });
            std::array<NF_RULE, 3> rules{};
            rules[0].processId = GetCurrentProcessId();
            rules[0].direction = NF_D_BOTH; rules[0].filteringFlag = NF_ALLOW;
            rules[1].protocol = IPPROTO_TCP; rules[1].direction = NF_D_OUT;
            rules[1].filteringFlag = NF_INDICATE_CONNECT_REQUESTS;
            rules[2].protocol = IPPROTO_UDP; rules[2].direction = NF_D_BOTH;
            rules[2].filteringFlag = NF_FILTER;
            const auto ruleStatus = nf_setRules(rules.data(), static_cast<int>(rules.size()));
            if (ruleStatus != NF_STATUS_SUCCESS) throw std::runtime_error(tr(Msg::RtFilteringRules, {std::to_string(ruleStatus)}));
        } catch (...) { stop(); throw; }
    }

    void stop() noexcept {
        stopping.store(true);
        wake.notify_all();
        // Publish cancellation before joining: all socket waits poll it.
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& item : tcp) item.second->cancel.store(true);
            for (auto& item : udp) item.second->cancel.store(true);
        }
        if (reaper.joinable()) reaper.join();
        std::array<std::thread, MaxTcpWorkers + MaxUdpWorkers> threads;
        std::size_t threadCount = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& item : tcp) if (item.second->thread.joinable()) threads[threadCount++] = std::move(item.second->thread);
            for (auto& item : udp) if (item.second->thread.joinable()) threads[threadCount++] = std::move(item.second->thread);
        }
        for (std::size_t i = 0; i < threadCount; ++i) threads[i].join();
        if (initialized) {
            nf_deleteRules();
            // SDK callbacks and their maps remain valid until nf_free returns.
            nf_free();
            initialized = false;
        }
        Impl* expected = this;
        active.compare_exchange_strong(expected, nullptr);
        std::lock_guard<std::mutex> lock(mutex);
        tcp.clear(); udp.clear(); udpWorkers = 0; tcpProxies = 0; udpProxies = 0;
    }
};

std::atomic<Engine::Impl*> Engine::Impl::active{nullptr};
Engine::Engine(const Config& config, ProcessTracker& tracker) : impl_(std::make_unique<Impl>(config, tracker)) {}
Engine::~Engine() { impl_->stop(); }
void Engine::start() { impl_->start(); }
void Engine::stop() { impl_->stop(); }
}
