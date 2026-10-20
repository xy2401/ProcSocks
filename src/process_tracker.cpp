#include "process_tracker.hpp"
#include "l10n.hpp"
#include "process_graph.hpp"
#include "log.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ps {
namespace {
constexpr GUID processProvider = {0x22fb2cd6, 0x0e7b, 0x422b, {0xa0,0xc7,0x2f,0xad,0x1f,0xd0,0xe7,0x16}};
constexpr wchar_t sessionName[] = L"ProcSocks.ProcessTracking";
constexpr std::uint64_t ticksPerSecond = 10000000;
std::uint64_t timestamp(FILETIME value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
std::uint64_t now() { FILETIME f{}; GetSystemTimeAsFileTime(&f); return timestamp(f); }
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = other.value; other.value = nullptr;
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Probe {
    ProcessIdentity identity;
    std::wstring image;
    bool fullImagePath = false;
    std::uint64_t ended = 0;
    Handle handle;
};
std::optional<Probe> probe(std::uint32_t pid, bool withImage = true) {
    Probe p;
    p.handle = Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!p.handle.value) return {};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(p.handle.value, &created, &exited, &kernel, &user)) return {};
    p.identity = {pid, timestamp(created)};
    p.ended = timestamp(exited);
    if (!withImage) return p;
    std::vector<wchar_t> path(32768);
    DWORD count = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(p.handle.value, 0, path.data(), &count)) {
        p.image.assign(path.data(), count);
        p.fullImagePath = true;
    }
    return p;
}
struct NativeProcess {
    ProcessIdentity identity;
    std::uint32_t parentPid = 0;
    std::wstring image;
};
std::map<std::uint32_t, NativeProcess> native_failure() {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) log_error(tr(Msg::RtMetadataUnavailable));
    return {};
}
std::map<std::uint32_t, NativeProcess> native_snapshot() {
    // Read-only fallback for protected processes: the system snapshot exposes
    // identity and basename even when OpenProcess cannot return a full path.
    using Query = NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static auto query = [] {
        auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
        Query function = nullptr;
        static_assert(sizeof(function) == sizeof(address), "Windows function pointer size");
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }();
    if (!query) return native_failure();
    std::vector<unsigned char> buffer(1024 * 1024);
    ULONG length = 0;
    NTSTATUS result = static_cast<NTSTATUS>(0xc0000004L);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        result = query(SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &length);
        if (result >= 0) break;
        if (static_cast<ULONG>(result) != 0xc0000004UL) return native_failure();
        const auto size = std::max<std::size_t>(buffer.size() * 2, static_cast<std::size_t>(length) + 65536);
        if (size > 64 * 1024 * 1024) return native_failure();
        buffer.resize(size);
    }
    if (result < 0) return native_failure();
    std::map<std::uint32_t, NativeProcess> processes;
    std::size_t offset = 0;
    const auto valid = length ? std::min<std::size_t>(length, buffer.size()) : buffer.size();
    while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= valid) {
        const auto* value = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
        auto pid = reinterpret_cast<std::uintptr_t>(value->UniqueProcessId);
        auto parent = reinterpret_cast<std::uintptr_t>(value->InheritedFromUniqueProcessId);
        if (pid <= UINT32_MAX && parent <= UINT32_MAX && value->CreateTime.QuadPart > 0) {
            NativeProcess p{{static_cast<std::uint32_t>(pid), static_cast<std::uint64_t>(value->CreateTime.QuadPart)},
                static_cast<std::uint32_t>(parent), {}};
            auto begin = reinterpret_cast<std::uintptr_t>(buffer.data());
            auto image = reinterpret_cast<std::uintptr_t>(value->ImageName.Buffer);
            auto bytes = value->ImageName.Length;
            if (bytes && bytes % sizeof(wchar_t) == 0 && image >= begin && image - begin <= valid &&
                bytes <= valid - (image - begin)) p.image.assign(value->ImageName.Buffer, bytes / sizeof(wchar_t));
            processes.emplace(p.identity.pid, std::move(p));
        }
        if (!value->NextEntryOffset) break;
        if (value->NextEntryOffset < sizeof(SYSTEM_PROCESS_INFORMATION) || value->NextEntryOffset > valid - offset) return native_failure();
        offset += value->NextEntryOffset;
    }
    return processes.empty() ? native_failure() : processes;
}
bool begins(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && CompareStringOrdinal(s.data(), static_cast<int>(prefix.size()),
        prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}
std::wstring dos_image(std::wstring path) {
    if (begins(path, L"\\??\\UNC\\")) return L"\\\\" + path.substr(8);
    if (begins(path, L"\\??\\")) path.erase(0, 4);
    if (begins(path, L"\\Device\\Mup\\")) return L"\\\\" + path.substr(12);
    if (begins(path, L"\\SystemRoot\\")) {
        wchar_t directory[MAX_PATH]{};
        auto length = GetWindowsDirectoryW(directory, MAX_PATH);
        if (length && length < MAX_PATH) return std::wstring(directory) + path.substr(11);
    }
    if (!begins(path, L"\\Device\\")) return path;
    wchar_t drives[512]{};
    if (!GetLogicalDriveStringsW(511, drives)) return path;
    for (auto drive = drives; *drive; drive += wcslen(drive) + 1) {
        std::wstring letter(drive, 2);
        wchar_t target[32768]{};
        if (QueryDosDeviceW(letter.c_str(), target, 32768)) {
            std::wstring prefix(target);
            if (begins(path, prefix) && path.size() > prefix.size() && path[prefix.size()] == L'\\')
                return letter + path.substr(prefix.size());
        }
    }
    return path;
}
bool full_image(const std::wstring& path) {
    return (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') || begins(path, L"\\\\");
}
std::vector<unsigned char> property(EVENT_RECORD* event, const wchar_t* name) {
    PROPERTY_DATA_DESCRIPTOR desc{};
    desc.PropertyName = reinterpret_cast<ULONGLONG>(name);
    desc.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    if (TdhGetPropertySize(event, 0, nullptr, 1, &desc, &size) != ERROR_SUCCESS || size > 65536) return {};
    std::vector<unsigned char> data(size);
    if (!size || TdhGetProperty(event, 0, nullptr, 1, &desc, size, data.data()) != ERROR_SUCCESS) return {};
    return data;
}
template<typename T> bool scalar(EVENT_RECORD* event, const wchar_t* name, T& output) {
    auto data = property(event, name);
    if (data.size() != sizeof(T)) return false;
    std::memcpy(&output, data.data(), sizeof(T));
    return true;
}
std::wstring image_property(EVENT_RECORD* event) {
    auto bytes = property(event, L"ImageName");
    if (bytes.empty() || bytes.size() % sizeof(wchar_t)) return {};
    std::wstring value(bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(value.data(), bytes.data(), bytes.size());
    auto end = value.find(L'\0');
    if (end != std::wstring::npos) value.resize(end);
    return dos_image(std::move(value));
}
std::vector<unsigned char> trace_properties() {
    std::vector<unsigned char> bytes(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(sessionName));
    auto p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(bytes.data());
    p->Wnode.BufferSize = static_cast<ULONG>(bytes.size());
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;
    p->BufferSize = 64;
    p->MinimumBuffers = 4;
    p->MaximumBuffers = 32;
    p->FlushTimer = 1;
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return bytes;
}
}

bool verify_process_metadata(std::string& details) {
    auto records = native_snapshot();
    auto current = probe(GetCurrentProcessId());
    if (!current) { details = "GetProcessTimes failed for test process"; return false; }
    auto found = records.find(current->identity.pid);
    if (found == records.end() || !(found->second.identity == current->identity)) {
        details = "Native creation time differs from GetProcessTimes"; return false;
    }
    if (!ProcessGraph::image_matches(found->second.image, current->image)) {
        details = "Native basename differs from QueryFullProcessImageName"; return false;
    }
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) { details = "Toolhelp snapshot failed"; return false; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool sawCurrent = false;
    if (Process32FirstW(snapshot.value, &entry)) {
        do {
            if (entry.th32ProcessID == current->identity.pid) {
                sawCurrent = true;
                if (entry.th32ParentProcessID != found->second.parentPid) {
                    details = "Native parent PID differs from Toolhelp"; return false;
                }
                break;
            }
        } while (Process32NextW(snapshot.value, &entry));
    }
    if (!sawCurrent) { details = "Toolhelp did not contain test process"; return false; }
    auto parent = probe(found->second.parentPid);
    auto parentRecord = records.find(found->second.parentPid);
    if (!parent || parentRecord == records.end() || !(parent->identity == parentRecord->second.identity) ||
        parent->identity.created > current->identity.created) {
        details = "Cannot verify current shell parent creation time"; return false;
    }
    ProcessGraph graph({L"C:\\ProcSocks-impossible-smoke-target-91A37.exe"}, true);
    graph.observe({parent->identity, 0, parentRecord->second.image, false, {}, true});
    graph.observe({current->identity, parent->identity.pid, found->second.image, false, parent->identity, false});
    if (graph.classify(current->identity) != ProcessDecision::Direct) {
        details = "Actual process metadata did not resolve a non-target process"; return false;
    }
    details = "Native snapshot verified " + std::to_string(records.size()) + " processes; self PID " +
        std::to_string(current->identity.pid) + " and shell parent PID " + std::to_string(parent->identity.pid) +
        " match GetProcessTimes, QueryFullProcessImageName and Toolhelp";
    return true;
}

struct ProcessTracker::Impl {
    explicit Impl(const Config& c) : graph(c.processes, c.includeChildren) {}
    ProcessGraph graph;
    std::mutex mutex;
    std::mutex snapshotMutex;
    std::condition_variable changed;
    std::atomic<bool> stopping{true};
    std::atomic<bool> healthy{false};
    std::atomic<bool> rebuild{false};
    TRACEHANDLE session = 0;
    TRACEHANDLE consumer = INVALID_PROCESSTRACE_HANDLE;
    std::thread consumerThread, maintenanceThread;
    std::map<ProcessIdentity, Handle> watched;
    std::uint64_t trackingStarted = 0;
    std::chrono::steady_clock::time_point lastSnapshot{};
    ULONG lostEvents = 0, lostBuffers = 0;

    void watch(Probe& p) {
        if (p.ended) graph.exited(p.identity, p.ended);
        else if (p.handle.value && !watched.count(p.identity)) watched.emplace(p.identity, std::move(p.handle));
    }
    static void WINAPI event_callback(EVENT_RECORD* event) noexcept {
        auto* self = static_cast<Impl*>(event->UserContext);
        if (!self || self->stopping) return;
        try { self->event(event); }
        catch (...) { self->rebuild = true; }
    }
    static ULONG WINAPI buffer_callback(EVENT_TRACE_LOGFILEW* file) {
        auto* self = static_cast<Impl*>(file->Context);
        return self && !self->stopping ? TRUE : FALSE;
    }
    void event(EVENT_RECORD* record) {
        if (!IsEqualGUID(record->EventHeader.ProviderId, processProvider)) return;
        auto eventId = record->EventHeader.EventDescriptor.Id;
        if (eventId != 1 && eventId != 2) return;
        std::uint32_t pid = 0;
        std::uint64_t created = 0;
        if (!scalar(record, L"ProcessID", pid) || !scalar(record, L"CreateTime", created)) {
            rebuild = true;
            return;
        }
        if (eventId == 2) {
            std::uint64_t ended = 0;
            if (!scalar(record, L"ExitTime", ended)) { rebuild = true; return; }
            std::lock_guard<std::mutex> lock(mutex);
            graph.exited({pid, created}, ended);
            watched.erase({pid, created});
        } else {
            std::uint32_t parentPid = 0;
            if (!scalar(record, L"ParentProcessID", parentPid)) { rebuild = true; return; }
            ProcessObservation observation{{pid, created}, parentPid, image_property(record), false, {}, false};
            auto current = probe(pid);
            auto parent = probe(parentPid);
            if (current && current->identity == observation.identity && !current->image.empty()) observation.image = current->image;
            observation.fullImagePath = full_image(observation.image);
            if (parent && parent->identity.created <= created && (!parent->ended || parent->ended >= created))
                observation.verifiedParent = parent->identity;
            std::lock_guard<std::mutex> lock(mutex);
            graph.observe(observation);
            if (current && current->identity == observation.identity) watch(*current);
            if (parent) watch(*parent);
        }
        changed.notify_all();
    }
    void snapshot(bool force = false) {
        std::unique_lock<std::mutex> scanLock(snapshotMutex, std::try_to_lock);
        if (!scanLock.owns_lock()) return;
        auto moment = std::chrono::steady_clock::now();
        if (!force && moment - lastSnapshot < std::chrono::milliseconds(150)) return;
        lastSnapshot = moment;
        const auto snapshotStarted = now();
        Handle handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (handle.value == INVALID_HANDLE_VALUE) throw std::runtime_error(tr(Msg::RtEnumerateProcesses, {std::to_string(GetLastError())}));
        std::map<DWORD, PROCESSENTRY32W> entries;
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (!Process32FirstW(handle.value, &entry)) throw std::runtime_error(tr(Msg::RtSnapshotRead));
        do { entries.emplace(entry.th32ProcessID, entry); } while (Process32NextW(handle.value, &entry));
        std::map<DWORD, Probe> probes;
        const auto native = native_snapshot();
        for (const auto& [pid, ignored] : entries) {
            (void)ignored;
            if (stopping) return;
            auto p = probe(pid);
            if (p) probes.emplace(pid, std::move(*p));
            else {
                auto it = native.find(pid);
                if (it != native.end()) {
                    Probe protectedProcess;
                    protectedProcess.identity = it->second.identity;
                    protectedProcess.image = it->second.image;
                    probes.emplace(pid, std::move(protectedProcess));
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [pid, p] : probes) {
            const auto& item = entries.at(pid);
            ProcessObservation o{p.identity, item.th32ParentProcessID, p.image, p.fullImagePath, {}, false};
            auto authoritative = native.find(pid);
            if (authoritative != native.end() && authoritative->second.identity == p.identity)
                o.parentPid = authoritative->second.parentPid;
            // A PID may be replaced while taking a snapshot. Never use the old
            // PROCESSENTRY32 parent for the new process instance.
            if (p.identity.created > snapshotStarted) continue;
            if (o.image.empty()) o.image = item.szExeFile;
            auto parent = probes.find(o.parentPid);
            if (parent != probes.end() && parent->second.identity.created <= o.identity.created &&
                (!parent->second.ended || parent->second.ended >= o.identity.created)) o.verifiedParent = parent->second.identity;
            if (o.identity.created < trackingStarted &&
                (!entries.count(o.parentPid) || (parent != probes.end() && parent->second.identity.created > o.identity.created)))
                o.ancestryBoundary = true;
            graph.observe(o);
            watch(p);
        }
        changed.notify_all();
    }
    void maintenance() noexcept {
        try {
            while (!stopping) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait_for(lock, std::chrono::seconds(1), [&] { return stopping.load(); });
                    if (stopping) return;
                    for (auto it = watched.begin(); it != watched.end();) {
                        FILETIME created{}, exited{}, kernel{}, user{};
                        if (GetProcessTimes(it->second.value, &created, &exited, &kernel, &user) && timestamp(exited)) {
                            graph.exited(it->first, timestamp(exited));
                            it = watched.erase(it);
                        } else ++it;
                    }
                    auto current = now();
                    graph.prune(current > 60 * ticksPerSecond ? current - 60 * ticksPerSecond : 0);
                }
                auto bytes = trace_properties();
                auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(bytes.data());
                auto status = ControlTraceW(session, nullptr, properties, EVENT_TRACE_CONTROL_QUERY);
                if (status != ERROR_SUCCESS && !stopping) {
                    healthy = false;
                    log_error(tr(Msg::RtProcessSessionUnavailable, {std::to_string(status)}));
                    changed.notify_all();
                    return;
                }
                bool lost = properties->EventsLost != lostEvents || properties->RealTimeBuffersLost != lostBuffers;
                lostEvents = properties->EventsLost;
                lostBuffers = properties->RealTimeBuffersLost;
                if (lost || rebuild.exchange(false)) {
                    log_error(tr(Msg::RtEventsLost));
                    snapshot(true);
                }
            }
        } catch (const std::exception& e) {
            healthy = false;
            log_error(tr(Msg::RtTrackingFailure, {e.what()}));
            changed.notify_all();
        }
    }
    void start() {
        if (!stopping) return;
        stopping = false;
        trackingStarted = now();
        auto bytes = trace_properties();
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(bytes.data());
        auto status = StartTraceW(&session, sessionName, properties);
        if (status != ERROR_SUCCESS) {
            session = 0; stopping = true;
            throw std::runtime_error(tr(Msg::RtEtwStart, {std::to_string(status)}));
        }
        try {
            status = EnableTraceEx2(session, &processProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                TRACE_LEVEL_INFORMATION, 0x10, 0, 0, nullptr);
            if (status != ERROR_SUCCESS) throw std::runtime_error(tr(Msg::RtEtwEnable, {std::to_string(status)}));
            EVENT_TRACE_LOGFILEW file{};
            file.LoggerName = const_cast<LPWSTR>(sessionName);
            file.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
            file.EventRecordCallback = &Impl::event_callback;
            file.BufferCallback = &Impl::buffer_callback;
            file.Context = this;
            consumer = OpenTraceW(&file);
            if (consumer == INVALID_PROCESSTRACE_HANDLE) throw std::runtime_error(tr(Msg::RtEtwOpen, {std::to_string(GetLastError())}));
            healthy = true;
            consumerThread = std::thread([this] {
                TRACEHANDLE handle = consumer;
                auto result = ProcessTrace(&handle, 1, nullptr, nullptr);
                if (!stopping) {
                    healthy = false;
                    log_error(tr(Msg::RtEtwEnded, {std::to_string(result)}));
                    changed.notify_all();
                }
            });
            snapshot(true);
            if (!healthy) throw std::runtime_error(tr(Msg::RtEtwInitializationStopped));
            maintenanceThread = std::thread([this] { maintenance(); });
            log_info(tr(Msg::RtTrackingReady));
        } catch (...) { stop(); throw; }
    }
    void stop() {
        stopping = true;
        healthy = false;
        changed.notify_all();
        if (consumer != INVALID_PROCESSTRACE_HANDLE) {
            // CloseTrace cancels a real-time ProcessTrace consumer; stopping our
            // own session also wakes a consumer when no further events arrive.
            CloseTrace(consumer);
        }
        if (maintenanceThread.joinable()) maintenanceThread.join();
        if (session) {
            auto bytes = trace_properties();
            ControlTraceW(session, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(bytes.data()), EVENT_TRACE_CONTROL_STOP);
            session = 0;
        }
        if (consumerThread.joinable()) consumerThread.join();
        consumer = INVALID_PROCESSTRACE_HANDLE;
        std::lock_guard<std::mutex> lock(mutex);
        watched.clear();
    }
    ProcessDecision classify(std::uint32_t pid, std::chrono::milliseconds timeout) {
        if (pid == GetCurrentProcessId()) return ProcessDecision::Direct;
        if (stopping) return ProcessDecision::Unknown;
        auto current = probe(pid, false);
        if (current && current->ended) return ProcessDecision::Unknown;
        if (timeout.count() <= 0 || !healthy) {
            if (!current) return ProcessDecision::Unknown;
            std::lock_guard<std::mutex> lock(mutex);
            // When the ETW stream fails, already proven instances retain their
            // decision. New/unverifiable instances remain Unknown; this avoids
            // interrupting unrelated established process trees unnecessarily.
            return !stopping ? graph.classify(current->identity) : ProcessDecision::Unknown;
        }
        ProcessIdentity identity;
        if (current) identity = current->identity;
        else {
            auto native = native_snapshot();
            auto it = native.find(pid);
            if (it == native.end()) return ProcessDecision::Unknown;
            identity = it->second.identity;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!healthy || stopping) return ProcessDecision::Unknown;
                auto decision = graph.classify(identity);
                if (decision != ProcessDecision::Unknown) return decision;
            }
            try { snapshot(); }
            catch (const std::exception& e) { log_error(tr(Msg::RtSnapshotFailure, {e.what()})); }
            std::unique_lock<std::mutex> lock(mutex);
            auto decision = graph.classify(identity);
            if (decision != ProcessDecision::Unknown && healthy && !stopping) return decision;
            changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(25)));
        } while (healthy && !stopping && std::chrono::steady_clock::now() < deadline);
        log_debug(tr(Msg::RtUnclassifiedPid, {std::to_string(pid)}));
        return ProcessDecision::Unknown;
    }
};
ProcessTracker::ProcessTracker(const Config& config) : impl_(std::make_unique<Impl>(config)) {}
ProcessTracker::~ProcessTracker() { impl_->stop(); }
void ProcessTracker::start() { impl_->start(); }
void ProcessTracker::stop() { impl_->stop(); }
ProcessDecision ProcessTracker::classify(std::uint32_t pid, std::chrono::milliseconds timeout) { return impl_->classify(pid, timeout); }
}
