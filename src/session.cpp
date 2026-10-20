#include "net.hpp" // Winsock2 must precede windows.h.
#include "l10n.hpp"
#include "session.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "process_tracker.hpp"
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace ps {
namespace {
class Event {
public:
    explicit Event(bool signalled = false) : value_(CreateEventW(nullptr, TRUE, signalled ? TRUE : FALSE, nullptr)) {
        if (!value_) throw std::runtime_error(tr(Msg::RtSessionEvent, {std::to_string(GetLastError())}));
    }
    ~Event() { CloseHandle(value_); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};
struct SharedLock {
    SRWLOCK& lock;
    explicit SharedLock(SRWLOCK& value) : lock(value) { AcquireSRWLockShared(&lock); }
    ~SharedLock() { ReleaseSRWLockShared(&lock); }
};
struct ExclusiveLock {
    SRWLOCK& lock;
    explicit ExclusiveLock(SRWLOCK& value) : lock(value) { AcquireSRWLockExclusive(&lock); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock); }
};
class WinsockScope {
public:
    WinsockScope() {
        WSADATA data{};
        const auto status = WSAStartup(MAKEWORD(2, 2), &data);
        if (status) throw std::runtime_error(tr(Msg::RtWinsockStartup, {std::to_string(status)}));
    }
    ~WinsockScope() { WSACleanup(); }
};
void wait_checked(HANDLE event, Msg action) {
    if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) {
        const auto error = GetLastError();
        throw std::runtime_error(tr(Msg::RtSessionWait, {tr(action), std::to_string(error)}));
    }
}
}

struct ControlSignals::Impl {
    Event stop;
    Event done{true};
    mutable SRWLOCK stateLock = SRWLOCK_INIT;
    bool operation = false;
    bool exiting = false;
    static SRWLOCK lifetimeLock;
    static Impl* active;

    static BOOL WINAPI console_handler(DWORD signal) noexcept {
        const bool control = signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT;
        const bool close = signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT || signal == CTRL_LOGOFF_EVENT;
        if (!control && !close) return FALSE;
        // Holding this shared lock pins the event handles until the handler has
        // left, including its bounded close wait. The destructor wakes done
        // before waiting for this lock, avoiding a shutdown lock inversion.
        SharedLock lifetime(lifetimeLock);
        auto* self = active;
        if (!self) return FALSE;
        {
            ExclusiveLock state(self->stateLock);
            if (close || !self->operation) self->exiting = true;
            SetEvent(self->stop.get());
        }
        if (close) WaitForSingleObject(self->done.get(), 4000);
        return TRUE;
    }
};
SRWLOCK ControlSignals::Impl::lifetimeLock = SRWLOCK_INIT;
ControlSignals::Impl* ControlSignals::Impl::active = nullptr;

ControlSignals::ControlSignals() : impl_(std::make_unique<Impl>()) {
    ExclusiveLock lifetime(Impl::lifetimeLock);
    if (Impl::active) throw std::runtime_error(tr(Msg::RtConsoleSingle));
    Impl::active = impl_.get();
    if (!SetConsoleCtrlHandler(&Impl::console_handler, TRUE)) {
        Impl::active = nullptr;
        throw std::runtime_error(tr(Msg::RtConsoleHandler, {std::to_string(GetLastError())}));
    }
}
ControlSignals::~ControlSignals() {
    SetEvent(impl_->done.get());
    SetConsoleCtrlHandler(&Impl::console_handler, FALSE);
    ExclusiveLock lifetime(Impl::lifetimeLock);
    if (Impl::active == impl_.get()) Impl::active = nullptr;
    // impl_ is destroyed after this local lock releases. New handlers see null;
    // all handlers that previously saw this instance have finished their use.
}
bool ControlSignals::begin_operation() {
    ExclusiveLock state(impl_->stateLock);
    if (impl_->operation || impl_->exiting || WaitForSingleObject(impl_->stop.get(), 0) == WAIT_OBJECT_0) return false;
    ResetEvent(impl_->done.get());
    impl_->operation = true;
    return true;
}
void ControlSignals::end_operation() {
    ExclusiveLock state(impl_->stateLock);
    if (impl_->operation) {
        impl_->operation = false;
        if (!impl_->exiting) ResetEvent(impl_->stop.get());
    }
    SetEvent(impl_->done.get());
}
void ControlSignals::request_stop() {
    ExclusiveLock state(impl_->stateLock);
    if (!impl_->operation) impl_->exiting = true;
    SetEvent(impl_->stop.get());
}
void ControlSignals::request_exit() {
    ExclusiveLock state(impl_->stateLock);
    impl_->exiting = true;
    SetEvent(impl_->stop.get());
}
bool ControlSignals::stop_requested() const { return WaitForSingleObject(impl_->stop.get(), 0) == WAIT_OBJECT_0; }
bool ControlSignals::exit_requested() const { SharedLock state(impl_->stateLock); return impl_->exiting; }
HANDLE ControlSignals::stop_event() const { return impl_->stop.get(); }

namespace {
class ProxyBackend final : public session_testing::Backend {
public:
    explicit ProxyBackend(const Config& config) : tracker_(config), engine_(config, tracker_), config_(config) {}
    void start_tracker() override { tracker_.start(); }
    void start_engine() override {
        engine_.start();
        log_info(tr(Msg::RtReady, {config_.includeChildren ? "true" : "false"}));
        log_info(tr(Msg::RtDemoLimit));
    }
    void stop_engine() override { engine_.stop(); }
    void stop_tracker() override { tracker_.stop(); }
private:
    WinsockScope winsock_;
    ProcessTracker tracker_;
    Engine engine_;
    const Config& config_;
};
}

void session_testing::run_with_backend(ControlSignals& signals, const Factory& factory, const SessionPump& pump) {
    if (!factory) throw std::invalid_argument(tr(Msg::RtSessionFactoryEmpty));
    Event completed;
    Event ready;
    std::exception_ptr workerFailure;
    std::thread worker([&] {
        std::unique_ptr<Backend> backend;
        try {
            if (!signals.stop_requested()) {
                backend = factory();
                if (!backend) throw std::runtime_error(tr(Msg::RtSessionFactoryNull));
            }
            if (backend && !signals.stop_requested()) backend->start_tracker();
            if (backend && !signals.stop_requested()) backend->start_engine();
            if (backend && !signals.stop_requested()) {
                SetEvent(ready.get());
                wait_checked(signals.stop_event(), Msg::RtWaitingCancellation);
            }
        } catch (...) { workerFailure = std::current_exception(); }
        if (backend) {
            try { backend->stop_engine(); }
            catch (...) { if (!workerFailure) workerFailure = std::current_exception(); }
            try { backend->stop_tracker(); }
            catch (...) { if (!workerFailure) workerFailure = std::current_exception(); }
            backend.reset();
        }
        SetEvent(completed.get());
    });
    std::exception_ptr pumpFailure;
    try {
        if (pump) pump(completed.get(), ready.get());
        else wait_checked(completed.get(), Msg::RtWaitingCompletion);
    } catch (...) { pumpFailure = std::current_exception(); }
    // Also handles a pump that returns early instead of waiting for completed.
    if (WaitForSingleObject(completed.get(), 0) != WAIT_OBJECT_0) signals.request_stop();
    worker.join();
    if (pumpFailure) std::rethrow_exception(pumpFailure);
    if (workerFailure) std::rethrow_exception(workerFailure);
}
bool session_testing::dispatch_control_signal(DWORD signal) { return ControlSignals::Impl::console_handler(signal) != FALSE; }

void run_proxy_session(const Config& config, ControlSignals& signals, const SessionPump& pump) {
    set_log_level(config.logLevel);
    session_testing::run_with_backend(signals, [&] { return std::make_unique<ProxyBackend>(config); }, pump);
}
}
