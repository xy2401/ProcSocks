#pragma once
#include "config.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <functional>
#include <memory>

namespace ps {
namespace session_testing { bool dispatch_control_signal(DWORD signal); }
// One owner per process. Keep this alive until every operation and its pump
// have returned. Ctrl+C stops an operation, or requests exit while idle.
class ControlSignals {
public:
    ControlSignals();
    ~ControlSignals();
    ControlSignals(const ControlSignals&) = delete;
    ControlSignals& operator=(const ControlSignals&) = delete;
    bool begin_operation();
    void end_operation();
    void request_stop();
    void request_exit();
    bool stop_requested() const;
    bool exit_requested() const;
    HANDLE stop_event() const;
private:
    friend bool session_testing::dispatch_control_signal(DWORD signal);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using SessionPump = std::function<void(HANDLE completed, HANDLE ready)>;

// Caller brackets this with begin_operation()/end_operation(). The worker
// owns a fresh tracker, engine and Winsock scope on every invocation. Neither
// this function nor the pump clears the caller's cancellation/operation state.
// Returning from a pump early cancels the session; pump errors cancel, join,
// then propagate. The default CLI pump waits for complete cleanup.
void run_proxy_session(const Config& config, ControlSignals& signals, const SessionPump& pump = {});

namespace session_testing {
// Injection seam for lifecycle tests; no driver or ETW session is required.
// Factories and backend methods run on the session worker, never the UI thread.
class Backend {
public:
    virtual ~Backend() = default;
    virtual void start_tracker() = 0;
    virtual void start_engine() = 0;
    virtual void stop_engine() = 0;
    virtual void stop_tracker() = 0;
};
using Factory = std::function<std::unique_ptr<Backend>()>;
void run_with_backend(ControlSignals& signals, const Factory& factory, const SessionPump& pump = {});
// Dispatches through the same handler as Windows, without delivering a signal
// to any other process sharing the test runner's console.
bool dispatch_control_signal(DWORD signal);
}
}
