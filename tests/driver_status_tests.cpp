#include "l10n.hpp"
// SCM transition tests use synthetic statuses and a virtual clock only. The
// optional --status-smoke command queries the actual service without changing it.
#include "../src/driver.cpp"
#include <iostream>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void fails(Function&& function, const std::string& expected) {
    try { function(); }
    catch (const std::exception& error) {
        check(std::string(error.what()).find(expected) != std::string::npos, "Unexpected failure detail");
        return;
    }
    throw std::runtime_error("Expected operation to fail");
}
struct FakeService {
    std::vector<DWORD> states;
    std::size_t cursor = 0;
    ULONGLONG clock = 0;
    unsigned starts = 0, reads = 0;
    bool failStart = false, failRead = false;
    DWORD exitCode = 0;
    void run() {
        ps::start_existing_service([&] {
            ++reads;
            if (failRead) throw std::runtime_error("query failed");
            SERVICE_STATUS result{};
            result.dwCurrentState = states.at(std::min(cursor++, states.size() - 1));
            result.dwWin32ExitCode = exitCode;
            return result;
        }, [&] {
            ++starts;
            if (failStart) throw std::runtime_error("start denied");
        }, [&] { return clock; }, [&](DWORD duration) {
            check(duration <= 100, "Poll interval exceeded 100ms");
            clock += duration;
        });
    }
};
const char* name(ps::DriverState state) {
    switch (state) {
        case ps::DriverState::NotInstalled: return "NotInstalled";
        case ps::DriverState::Stopped: return "Stopped";
        case ps::DriverState::Running: return "Running";
        case ps::DriverState::Pending: return "Pending";
    }
    return "invalid";
}
}
int main(int argc, char** argv) {
    ps::set_language(ps::Language::EnUS);
    try {
        if (argc == 2 && std::string(argv[1]) == "--status-smoke") {
            std::cout << "Read-only driver state: " << name(ps::query_driver_state()) << '\n';
            return 0;
        }
        const std::filesystem::path expected = L"C:\\Windows\\System32\\drivers\\procsocks_nf.sys";
        const auto owned = [&](DWORD type, const std::wstring& path) { return ps::service_identity_matches(type, path, expected, L"C:\\Windows"); };
        check(owned(SERVICE_KERNEL_DRIVER, expected.wstring()), "Absolute owned path");
        check(owned(SERVICE_KERNEL_DRIVER, L"system32\\drivers\\procsocks_nf.sys"), "SDK SystemRoot-relative path");
        check(owned(SERVICE_KERNEL_DRIVER, L"SYSTEM32/DRIVERS/PROCSOCKS_NF.SYS"), "Relative path case and separators");
        check(owned(SERVICE_KERNEL_DRIVER, L"\\systemroot\\system32\\drivers\\procsocks_nf.sys"), "SystemRoot native path");
        check(owned(SERVICE_KERNEL_DRIVER, L"\\??\\C:\\Windows\\System32\\drivers\\procsocks_nf.sys"), "Native DOS path");
        check(owned(SERVICE_KERNEL_DRIVER, L"\"C:\\Windows\\System32\\drivers\\procsocks_nf.sys\""), "Quoted absolute path");
        check(!owned(SERVICE_WIN32_OWN_PROCESS, expected.wstring()), "Foreign service type must be rejected");
        check(!owned(SERVICE_KERNEL_DRIVER, L"C:\\Other\\procsocks_nf.sys"), "Foreign absolute path must be rejected");
        check(!owned(SERVICE_KERNEL_DRIVER, L"system32\\drivers\\..\\..\\Other\\procsocks_nf.sys"), "Relative escape must be rejected");
        check(!owned(SERVICE_KERNEL_DRIVER, L"drivers\\procsocks_nf.sys"), "Unrecognized relative path must be rejected");
        check(!owned(SERVICE_KERNEL_DRIVER, L"C:\\Windows\\System32\\drivers\\procsocks_nf.sys -arg"), "Driver command arguments must be rejected");
        check(!owned(SERVICE_KERNEL_DRIVER, L"!C:\\Windows\\System32\\drivers\\procsocks_nf.sys"), "Pending-file replacement marker is not a service path");
        check(ps::driver_state(SERVICE_STOPPED) == ps::DriverState::Stopped, "Stopped mapping");
        check(ps::driver_state(SERVICE_RUNNING) == ps::DriverState::Running, "Running mapping");
        for (auto pending : {SERVICE_START_PENDING, SERVICE_STOP_PENDING, SERVICE_CONTINUE_PENDING, SERVICE_PAUSE_PENDING})
            check(ps::driver_state(pending) == ps::DriverState::Pending, "Pending mapping");
        fails([] { ps::driver_state(SERVICE_PAUSED); }, "Unsupported");
        fails([] { ps::driver_state(0); }, "Unsupported");
        FakeService running{{SERVICE_RUNNING}};
        running.run(); check(running.starts == 0 && running.clock == 0, "Already-running service was started again");
        FakeService stopped{{SERVICE_STOPPED, SERVICE_START_PENDING, SERVICE_RUNNING}};
        stopped.run(); check(stopped.starts == 1 && stopped.clock == 100, "Stopped service must start once");
        FakeService starting{{SERVICE_START_PENDING, SERVICE_RUNNING}};
        starting.run(); check(starting.starts == 0, "StartPending must be awaited without another start");
        FakeService stopping{{SERVICE_STOP_PENDING, SERVICE_STOPPED, SERVICE_START_PENDING, SERVICE_RUNNING}};
        stopping.run(); check(stopping.starts == 1 && stopping.clock == 200, "StopPending must complete before one start");
        FakeService failed{{SERVICE_STOPPED, SERVICE_STOPPED}}; failed.exitCode = ERROR_BAD_EXE_FORMAT;
        fails([&] { failed.run(); }, "Windows exit 193"); check(failed.starts == 1, "Failed driver was automatically restarted");
        FakeService preexistingFailed{{SERVICE_START_PENDING, SERVICE_STOPPED}};
        fails([&] { preexistingFailed.run(); }, "stopped during startup");
        check(preexistingFailed.starts == 0, "Another failed start must not trigger retry");
        FakeService stopAfterStart{{SERVICE_STOPPED, SERVICE_START_PENDING, SERVICE_STOP_PENDING, SERVICE_STOPPED}};
        fails([&] { stopAfterStart.run(); }, "stopped during startup"); check(stopAfterStart.starts == 1, "Stop during start caused retry");
        FakeService timeout{{SERVICE_STOP_PENDING}};
        fails([&] { timeout.run(); }, "10 seconds"); check(timeout.clock == 10000 && timeout.starts == 0, "Stop wait exceeded deadline or started early");
        FakeService startTimeout{{SERVICE_STOPPED, SERVICE_START_PENDING}};
        fails([&] { startTimeout.run(); }, "10 seconds"); check(startTimeout.clock == 10000 && startTimeout.starts == 1, "Start wait exceeded deadline");
        FakeService denied{{SERVICE_STOPPED}}; denied.failStart = true;
        fails([&] { denied.run(); }, "start denied"); check(denied.starts == 1 && denied.clock == 0, "Start failure retried");
        FakeService unreadable{{SERVICE_STOPPED}}; unreadable.failRead = true;
        fails([&] { unreadable.run(); }, "query failed"); check(unreadable.starts == 0, "Unreadable state attempted start");
        FakeService paused{{SERVICE_PAUSED}};
        fails([&] { paused.run(); }, "cannot be started"); check(paused.starts == 0, "Unsupported state attempted start");
        std::cout << "driver_status_tests: state and bounded-start transitions passed; no real service was changed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "driver_status_tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
