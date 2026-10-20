#include "l10n.hpp"
#include "session.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using ps::ControlSignals;
using ps::session_testing::Backend;
using ps::session_testing::dispatch_control_signal;
using ps::session_testing::run_with_backend;
int checks = 0;
void check(bool good, const char* label) { ++checks; if (!good) throw std::runtime_error(label); }
void wait(HANDLE event, const char* label) { check(WaitForSingleObject(event, 3000) == WAIT_OBJECT_0, label); }
struct Event {
    HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() { if (!value) throw std::runtime_error("test event creation failed"); }
    ~Event() { CloseHandle(value); }
};
struct State {
    std::mutex mutex;
    std::vector<std::string> calls;
    std::thread::id worker;
    std::atomic_int instances{0}, destroyed{0};
    Event trackerEntered, engineEntered, release;
    bool blockTracker = false, blockEngine = false;
    bool failTracker = false, failEngine = false, failStopEngine = false;
    std::function<void()> engineHook;
    void record(const char* name) {
        std::lock_guard<std::mutex> lock(mutex);
        if (worker == std::thread::id{}) worker = std::this_thread::get_id();
        if (worker != std::this_thread::get_id()) throw std::runtime_error("backend lifecycle changed worker thread");
        calls.emplace_back(name);
    }
};
class FakeBackend final : public Backend {
public:
    explicit FakeBackend(State& state) : s(state) { ++s.instances; s.record("construct"); }
    ~FakeBackend() override { ++s.destroyed; s.record("destroy"); }
    void start_tracker() override {
        s.record("tracker-start"); SetEvent(s.trackerEntered.value);
        if (s.blockTracker && WaitForSingleObject(s.release.value, 3000) != WAIT_OBJECT_0) throw std::runtime_error("tracker test gate timed out");
        if (s.failTracker) throw std::runtime_error("tracker startup failed");
    }
    void start_engine() override {
        s.record("engine-start"); SetEvent(s.engineEntered.value);
        if (s.blockEngine && WaitForSingleObject(s.release.value, 3000) != WAIT_OBJECT_0) throw std::runtime_error("engine test gate timed out");
        if (s.failEngine) throw std::runtime_error("engine startup failed");
        if (s.engineHook) s.engineHook();
    }
    void stop_engine() override { s.record("engine-stop"); if (s.failStopEngine) throw std::runtime_error("engine cleanup failed"); }
    void stop_tracker() override { s.record("tracker-stop"); }
private:
    State& s;
};
auto factory(State& state) { return [&state] { return std::make_unique<FakeBackend>(state); }; }
void verify_calls(State& state, const std::vector<std::string>& expected) {
    check(state.calls == expected, "backend lifecycle ordering");
    check(state.instances == 1 && state.destroyed == 1, "backend did not get a fresh owned lifetime");
    check(state.worker != std::this_thread::get_id(), "backend ran on UI thread");
}
void signals_test() {
    {
        ControlSignals signals;
        check(!signals.stop_requested() && !signals.exit_requested(), "initial signal state");
        check(!dispatch_control_signal(999), "unknown control event was consumed");
        bool duplicateRejected = false;
        try { ControlSignals duplicate; } catch (const std::exception&) { duplicateRejected = true; }
        check(duplicateRejected, "duplicate console handler owner accepted");
        check(signals.begin_operation(), "initial operation rejected");
        check(!signals.begin_operation(), "nested operation accepted");
        check(dispatch_control_signal(CTRL_C_EVENT), "Ctrl+C not handled");
        check(signals.stop_requested() && !signals.exit_requested(), "running Ctrl+C should stop without exiting");
        signals.end_operation();
        check(!signals.stop_requested() && !signals.exit_requested(), "per-operation stop not retired");
        check(signals.begin_operation(), "second operation rejected");
        check(dispatch_control_signal(CTRL_BREAK_EVENT), "Ctrl+Break not handled");
        signals.end_operation();
        check(dispatch_control_signal(CTRL_C_EVENT), "idle Ctrl+C not handled");
        check(signals.exit_requested() && signals.stop_requested(), "idle Ctrl+C should exit");
        check(!signals.begin_operation(), "pending idle Ctrl+C was lost");
        signals.end_operation();
        check(signals.exit_requested() && signals.stop_requested(), "end operation erased pending exit");
    }
    check(!dispatch_control_signal(CTRL_C_EVENT), "destroyed signal controller remained registered");
    {
        ControlSignals signals;
        signals.request_stop();
        check(!signals.begin_operation() && signals.exit_requested(), "idle stop was lost at begin_operation");
    }
}
void close_signal_test() {
    auto signals = std::make_unique<ControlSignals>();
    check(signals->begin_operation(), "close test operation");
    auto closing = std::async(std::launch::async, [] { return dispatch_control_signal(CTRL_CLOSE_EVENT); });
    wait(signals->stop_event(), "close did not request stop");
    check(signals->exit_requested(), "close did not request exit");
    check(closing.wait_for(0ms) == std::future_status::timeout, "close handler did not wait for cleanup");
    signals->end_operation();
    check(closing.wait_for(1s) == std::future_status::ready && closing.get(), "cleanup did not wake close handler");
    check(!signals->begin_operation(), "close exit discarded by end_operation");
    signals.reset();

    // Destruction must wake an in-flight close handler before retiring handles.
    signals = std::make_unique<ControlSignals>();
    check(signals->begin_operation(), "destruction test operation");
    auto shutdown = std::async(std::launch::async, [] { return dispatch_control_signal(CTRL_SHUTDOWN_EVENT); });
    wait(signals->stop_event(), "shutdown did not request stop");
    signals.reset();
    check(shutdown.wait_for(1s) == std::future_status::ready && shutdown.get(), "controller destruction raced handler handle lifetime");
}
void repeated_session_test() {
    ControlSignals signals;
    for (int attempt = 0; attempt != 3; ++attempt) {
        State state;
        check(signals.begin_operation(), "repeated session begin");
        run_with_backend(signals, factory(state), [&](HANDLE completed, HANDLE ready) {
            wait(ready, "session never became ready");
            signals.request_stop();
            wait(completed, "session did not finish cleanup");
            check(state.destroyed == 1, "completed signalled before backend destruction");
        });
        check(signals.stop_requested(), "session runner cleared caller cancellation");
        signals.end_operation();
        verify_calls(state, {"construct", "tracker-start", "engine-start", "engine-stop", "tracker-stop", "destroy"});
    }
}
void startup_failure_retry_test() {
    ControlSignals signals;
    for (int failure = 0; failure != 3; ++failure) {
        State state;
        state.failTracker = failure == 0;
        state.failEngine = failure == 1;
        bool failed = false;
        check(signals.begin_operation(), "startup failure retry begin");
        try {
            run_with_backend(signals, factory(state), [&](HANDLE completed, HANDLE ready) {
                if (failure != 2) {
                    wait(completed, "failed initialization did not complete");
                    check(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT, "failed initialization announced ready");
                } else {
                    wait(ready, "retry did not start"); signals.request_stop(); wait(completed, "retry did not stop");
                }
            });
        } catch (const std::runtime_error& e) {
            failed = std::string(e.what()) == (failure == 0 ? "tracker startup failed" : "engine startup failed");
        }
        check(failed == (failure != 2), "startup failure propagation or retry");
        signals.end_operation();
        verify_calls(state, failure == 0 ? std::vector<std::string>{"construct", "tracker-start", "engine-stop", "tracker-stop", "destroy"} :
            std::vector<std::string>{"construct", "tracker-start", "engine-start", "engine-stop", "tracker-stop", "destroy"});
    }
}
void startup_cancel_test() {
    ControlSignals signals;
    for (int stage = 0; stage != 2; ++stage) {
        State state; state.blockTracker = stage == 0; state.blockEngine = stage == 1;
        check(signals.begin_operation(), "startup cancellation begin");
        run_with_backend(signals, factory(state), [&](HANDLE completed, HANDLE ready) {
            wait(stage == 0 ? state.trackerEntered.value : state.engineEntered.value, "startup hook not reached");
            check(dispatch_control_signal(CTRL_C_EVENT), "startup Ctrl+C not handled");
            SetEvent(state.release.value);
            wait(completed, "cancelled startup did not finish");
            check(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT, "cancelled startup announced ready");
        });
        check(signals.stop_requested() && !signals.exit_requested(), "startup cancellation disappeared");
        signals.end_operation();
        verify_calls(state, stage == 0 ? std::vector<std::string>{"construct", "tracker-start", "engine-stop", "tracker-stop", "destroy"} :
            std::vector<std::string>{"construct", "tracker-start", "engine-start", "engine-stop", "tracker-stop", "destroy"});
    }
    State state;
    check(signals.begin_operation(), "pre-cancel begin");
    signals.request_stop();
    run_with_backend(signals, factory(state), [&](HANDLE completed, HANDLE ready) {
        wait(completed, "pre-cancelled session did not return");
        check(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT, "pre-cancelled session became ready");
    });
    check(state.instances == 0, "pre-cancellation still created backend");
    signals.end_operation();
}
void pump_and_cleanup_failure_test() {
    ControlSignals signals;
    State pumpState;
    check(signals.begin_operation(), "pump failure begin");
    bool pumpError = false;
    try {
        run_with_backend(signals, factory(pumpState), [&](HANDLE, HANDLE ready) {
            wait(ready, "pump failure setup"); throw std::runtime_error("UI pump failed");
        });
    } catch (const std::runtime_error& e) { pumpError = std::string(e.what()) == "UI pump failed"; }
    check(pumpError && pumpState.destroyed == 1, "pump failure did not cancel, join and propagate");
    signals.end_operation();
    verify_calls(pumpState, {"construct", "tracker-start", "engine-start", "engine-stop", "tracker-stop", "destroy"});

    State cleanupState; cleanupState.failStopEngine = true;
    check(signals.begin_operation(), "cleanup failure begin");
    bool cleanupError = false;
    try {
        run_with_backend(signals, factory(cleanupState), [&](HANDLE, HANDLE ready) { wait(ready, "cleanup setup"); signals.request_stop(); });
    } catch (const std::runtime_error& e) { cleanupError = std::string(e.what()) == "engine cleanup failed"; }
    check(cleanupError, "cleanup failure was swallowed");
    signals.end_operation();
    verify_calls(cleanupState, {"construct", "tracker-start", "engine-start", "engine-stop", "tracker-stop", "destroy"});

    State earlyState;
    check(signals.begin_operation(), "early pump begin");
    run_with_backend(signals, factory(earlyState), [&](HANDLE, HANDLE ready) { wait(ready, "early pump setup"); });
    check(signals.stop_requested() && earlyState.destroyed == 1, "early pump return stranded worker");
    signals.end_operation();

    State defaultState;
    defaultState.engineHook = [&] { signals.request_stop(); };
    check(signals.begin_operation(), "default pump begin");
    run_with_backend(signals, factory(defaultState));
    check(defaultState.destroyed == 1, "default pump did not wait for cleanup");
    signals.end_operation();
}
}

int main() {
    ps::set_language(ps::Language::EnUS);
    try {
        signals_test();
        close_signal_test();
        repeated_session_test();
        startup_failure_retry_test();
        startup_cancel_test();
        pump_and_cleanup_failure_test();
        std::cout << "session_tests: " << checks << " checks passed (no driver or ETW loaded)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "session_tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
