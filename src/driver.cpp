#include "net.hpp"
#include "l10n.hpp"
#include <windows.h>
#include <winsvc.h>
#include "nfapi.h"
#include "driver.hpp"
#include "engine.hpp"
#include "log.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ps {
namespace {
class ServiceHandle {
public:
    explicit ServiceHandle(SC_HANDLE handle) : handle_(handle) {}
    ~ServiceHandle() { if (handle_) CloseServiceHandle(handle_); }
    operator SC_HANDLE() const { return handle_; }
private:
    SC_HANDLE handle_;
};
[[noreturn]] void win_error(Msg operation) {
    const auto error = GetLastError();
    throw std::runtime_error(tr(Msg::RtWindowsFailure, {tr(operation), std::to_string(error)}));
}
std::filesystem::path executable_directory() {
    std::vector<wchar_t> buffer(32768);
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) win_error(Msg::RtExecutableDirectory);
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path();
}
std::filesystem::path driver_path() {
    wchar_t buffer[MAX_PATH]{};
    const auto count = GetSystemDirectoryW(buffer, MAX_PATH);
    if (!count || count >= MAX_PATH) win_error(Msg::RtSystemDirectory);
    return std::filesystem::path(buffer) / L"drivers" / L"procsocks_nf.sys";
}
bool ordinal_equal(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
bool starts_with_ci(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() && ordinal_equal(value.substr(0, prefix.size()), prefix);
}
std::wstring pending_path(std::wstring value, const std::wstring& windowsDirectory) {
    // Session Manager prefixes replacement destinations with '!'. MoveFileExW
    // normally records native DOS paths (\??\C:\...), including for deletions.
    if (!value.empty() && value.front() == L'!') value.erase(0, 1);
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (starts_with_ci(value, L"\\??\\") || starts_with_ci(value, L"\\\\?\\")) value.erase(0, 4);
    else if (starts_with_ci(value, L"\\DosDevices\\")) value.erase(0, 12);
    if (starts_with_ci(value, L"UNC\\")) value = L"\\\\" + value.substr(4);
    if (starts_with_ci(value, L"\\SystemRoot\\")) value = windowsDirectory + L"\\" + value.substr(12);
    if (value.empty()) return {};
    return std::filesystem::path(value).lexically_normal().wstring();
}
bool pending_operations_touch_path(const std::vector<wchar_t>& data, const std::filesystem::path& expected,
                                  const std::wstring& windowsDirectory) {
    const auto target = pending_path(expected.wstring(), windowsDirectory);
    std::size_t offset = 0;
    const auto read_string = [&]() {
        const auto begin = offset;
        while (offset < data.size() && data[offset]) ++offset;
        if (offset == data.size()) throw std::runtime_error(tr(Msg::RtPendingMalformed));
        std::wstring result(data.data() + begin, offset - begin);
        ++offset;
        return result;
    };
    while (offset < data.size()) {
        if (!data[offset]) {
            // An empty source terminates the list; an empty destination within
            // a pair means delete. Do not stop parsing after another file's delete.
            if (std::any_of(data.begin() + static_cast<std::ptrdiff_t>(offset), data.end(), [](wchar_t c) { return c != 0; }))
                throw std::runtime_error(tr(Msg::RtPendingMalformed));
            return false;
        }
        const auto source = read_string();
        const auto destination = read_string();
        if (ordinal_equal(pending_path(source, windowsDirectory), target) ||
            (!destination.empty() && ordinal_equal(pending_path(destination, windowsDirectory), target))) return true;
    }
    return false;
}
void ensure_no_pending_driver_operation(const std::filesystem::path& expected) {
    HKEY key = nullptr;
    LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0,
                                   KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (result == ERROR_FILE_NOT_FOUND) return;
    if (result != ERROR_SUCCESS) throw std::runtime_error(tr(Msg::RtPendingInspect, {std::to_string(result)}));
    struct KeyCloser { HKEY value; ~KeyCloser() { RegCloseKey(value); } } closer{key};
    wchar_t windows[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!length || length >= MAX_PATH) win_error(Msg::RtWindowsDirectory);
    // Retry a bounded number of times if another installer grows the value while
    // it is read. This guard never creates, deletes, or changes registry values.
    for (int attempt = 0; attempt != 3; ++attempt) {
        DWORD type = 0, bytes = 0;
        result = RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, nullptr, &bytes);
        if (result == ERROR_FILE_NOT_FOUND) return;
        if (result != ERROR_SUCCESS) throw std::runtime_error(tr(Msg::RtPendingRead, {std::to_string(result)}));
        if (type != REG_MULTI_SZ || bytes % sizeof(wchar_t) || bytes > 1024 * 1024)
            throw std::runtime_error(tr(Msg::RtPendingInvalid));
        if (!bytes) return;
        std::vector<wchar_t> data(bytes / sizeof(wchar_t));
        result = RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type,
                                  reinterpret_cast<BYTE*>(data.data()), &bytes);
        if (result == ERROR_MORE_DATA) continue;
        if (result == ERROR_FILE_NOT_FOUND) return;
        if (result != ERROR_SUCCESS) throw std::runtime_error(tr(Msg::RtPendingRead, {std::to_string(result)}));
        if (type != REG_MULTI_SZ || bytes % sizeof(wchar_t))
            throw std::runtime_error(tr(Msg::RtPendingInvalid));
        data.resize(bytes / sizeof(wchar_t));
        if (pending_operations_touch_path(data, expected, windows))
            throw std::runtime_error(tr(Msg::RtPendingDriver));
        return;
    }
    throw std::runtime_error(tr(Msg::RtPendingChanged));
}
bool same_file_contents(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (std::filesystem::file_size(a) != std::filesystem::file_size(b)) return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    if (!left || !right) throw std::runtime_error(tr(Msg::RtDriverCompare));
    char l[8192], r[8192];
    while (left.read(l, sizeof(l)) || left.gcount()) {
        auto length = left.gcount();
        right.read(r, length);
        if (right.gcount() != length || std::memcmp(l, r, static_cast<std::size_t>(length))) return false;
    }
    if (left.bad() || right.bad()) throw std::runtime_error(tr(Msg::RtDriverRead));
    return true;
}
bool service_identity_matches(DWORD type, std::wstring binaryPath, const std::filesystem::path& expected,
                              const std::wstring& windowsDirectory) {
    if (type != SERVICE_KERNEL_DRIVER) return false;
    if (binaryPath.size() >= 2 && binaryPath.front() == L'"' && binaryPath.back() == L'"')
        binaryPath = binaryPath.substr(1, binaryPath.size() - 2);
    if (binaryPath.empty() || binaryPath.front() == L'!') return false;
    // nf_registerDriver writes the kernel ImagePath as system32\drivers\... .
    // SCM interprets this specific relative form beneath the Windows directory.
    std::replace(binaryPath.begin(), binaryPath.end(), L'/', L'\\');
    if (starts_with_ci(binaryPath, L"system32\\")) binaryPath = windowsDirectory + L"\\" + binaryPath;
    return ordinal_equal(pending_path(std::move(binaryPath), windowsDirectory),
                         pending_path(expected.wstring(), windowsDirectory));
}
void validate_service(SC_HANDLE service, const std::filesystem::path& expected) {
    DWORD length = 0;
    QueryServiceConfigW(service, nullptr, 0, &length);
    if (!length) win_error(Msg::RtDriverQueryService);
    std::vector<unsigned char> buffer(length);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, config, length, &length)) win_error(Msg::RtDriverReadService);
    wchar_t windows[MAX_PATH]{};
    const auto count = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!count || count >= MAX_PATH) win_error(Msg::RtWindowsDirectory);
    if (!config->lpBinaryPathName || !service_identity_matches(config->dwServiceType, config->lpBinaryPathName, expected, windows))
        throw std::runtime_error(tr(Msg::RtDriverOtherInstallation));
}
SERVICE_STATUS read_service_status(SC_HANDLE service) {
    SERVICE_STATUS status{};
    if (!QueryServiceStatus(service, &status)) win_error(Msg::RtDriverQueryStatus);
    return status;
}
DriverState driver_state(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED: return DriverState::Stopped;
        case SERVICE_RUNNING: return DriverState::Running;
        case SERVICE_START_PENDING:
        case SERVICE_STOP_PENDING:
        case SERVICE_CONTINUE_PENDING:
        case SERVICE_PAUSE_PENDING: return DriverState::Pending;
        default: throw std::runtime_error(tr(Msg::RtDriverStateUnsupported, {std::to_string(state)}));
    }
}
// Keep transition handling independent of SCM so tests can exercise it without
// starting, stopping or installing any actual driver/service.
template<class ReadStatus, class RequestStart, class Now, class SleepFor>
void start_existing_service(ReadStatus&& readStatus, RequestStart&& requestStart, Now&& now, SleepFor&& sleepFor) {
    const auto deadline = now() + 10000;
    bool startRequested = false, observedStartPending = false;
    for (;;) {
        if (now() >= deadline)
            throw std::runtime_error(tr(Msg::RtDriverStartTimeout));
        const auto status = readStatus();
        if (status.dwCurrentState == SERVICE_RUNNING) return;
        switch (status.dwCurrentState) {
            case SERVICE_STOPPED:
                if (startRequested || observedStartPending)
                    throw std::runtime_error(tr(Msg::RtDriverStoppedStartup, {std::to_string(status.dwWin32ExitCode), std::to_string(status.dwServiceSpecificExitCode)}));
                requestStart();
                startRequested = true;
                continue;
            case SERVICE_START_PENDING:
                observedStartPending = true;
                break;
            case SERVICE_STOP_PENDING:
                // An existing stop must finish before starting the same service.
                // A stop after our start is a failure, never a restart loop.
                break;
            default:
                throw std::runtime_error(tr(Msg::RtDriverStartState, {std::to_string(status.dwCurrentState)}));
        }
        const auto current = now();
        if (current >= deadline) continue;
        sleepFor(static_cast<DWORD>(std::min<ULONGLONG>(100, deadline - current)));
    }
}
void require_admin() {
    if (!is_administrator()) throw std::runtime_error(tr(Msg::RtAdministratorRequired));
}
}
bool is_administrator() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) return false;
    BOOL member = FALSE;
    const BOOL success = CheckTokenMembership(nullptr, group, &member);
    FreeSid(group);
    return success && member;
}
DriverState query_driver_state() {
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) win_error(Msg::RtServiceManager);
    ServiceHandle service(OpenServiceW(scm, DriverNameW, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return DriverState::NotInstalled;
        win_error(Msg::RtDriverQueryService);
    }
    validate_service(service, driver_path());
    return driver_state(read_service_status(service).dwCurrentState);
}
void start_driver() {
    require_admin();
    const auto expected = driver_path();
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) win_error(Msg::RtServiceManager);
    ServiceHandle service(OpenServiceW(scm, DriverNameW, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START));
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            throw std::runtime_error(tr(Msg::RtDriverNotInstalled));
        win_error(Msg::RtDriverOpenService);
    }
    validate_service(service, expected);
    ensure_no_pending_driver_operation(expected);
    start_existing_service([&] { return read_service_status(service); }, [&] {
        if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
            win_error(Msg::RtDriverStartExisting);
    }, [] { return GetTickCount64(); }, [](DWORD duration) { Sleep(duration); });
    log_info(tr(Msg::RtDriverRunning));
}
void install_driver() {
    require_admin();
    auto source = executable_directory() / L"driver" / L"procsocks_nf.sys";
    auto destination = driver_path();
    if (!std::filesystem::is_regular_file(source)) throw std::runtime_error(tr(Msg::RtDriverMissingFile));
    ensure_no_pending_driver_operation(destination);
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) win_error(Msg::RtServiceManager);
    ServiceHandle service(OpenServiceW(scm, DriverNameW, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
    if (service) validate_service(service, destination);
    else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) win_error(Msg::RtDriverQueryExisting);
    bool copied = false;
    if (std::filesystem::exists(destination)) {
        if (!same_file_contents(source, destination)) throw std::runtime_error(tr(Msg::RtDriverDifferentFile));
    } else {
        if (!CopyFileW(source.c_str(), destination.c_str(), TRUE)) win_error(Msg::RtDriverCopy);
        copied = true;
    }
    const NF_STATUS status = nf_registerDriver(DriverName);
    if (status != NF_STATUS_SUCCESS) {
        const DWORD error = GetLastError();
        // Registration can partially succeed. Leave a registered service/file intact for explicit cleanup.
        ServiceHandle after(OpenServiceW(scm, DriverNameW, SERVICE_QUERY_CONFIG));
        if (copied && !after && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) DeleteFileW(destination.c_str());
        throw std::runtime_error(tr(Msg::RtDriverRegistration, {std::to_string(status), std::to_string(error)}));
    }
    log_info(tr(Msg::RtDriverInstalled));
}
void uninstall_driver() {
    require_admin();
    auto destination = driver_path();
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) win_error(Msg::RtServiceManager);
    {
        ServiceHandle service(OpenServiceW(scm, DriverNameW, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP));
        if (service) {
            validate_service(service, destination);
            SERVICE_STATUS status{};
            if (!ControlService(service, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
                win_error(Msg::RtDriverStop);
            const auto deadline = GetTickCount64() + 10000;
            while (true) {
                if (!QueryServiceStatus(service, &status)) win_error(Msg::RtDriverQueryStatus);
                if (status.dwCurrentState == SERVICE_STOPPED) break;
                if (GetTickCount64() >= deadline) throw std::runtime_error(tr(Msg::RtDriverStopTimeout));
                Sleep(100);
            }
        } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) win_error(Msg::RtDriverQueryService);
        else {
            if (std::filesystem::exists(destination)) {
                auto packaged = executable_directory() / L"driver" / L"procsocks_nf.sys";
                if (!std::filesystem::exists(packaged) || !same_file_contents(packaged, destination))
                    throw std::runtime_error(tr(Msg::RtDriverUnregisteredDifferentFile));
            }
            if (std::filesystem::exists(destination) && !DeleteFileW(destination.c_str())) win_error(Msg::RtDriverRemoveUnregistered);
            log_info(tr(Msg::RtDriverLeftoverRemoved));
            return;
        }
    }
    const auto status = nf_unRegisterDriver(DriverName);
    if (status != NF_STATUS_SUCCESS && status != NF_STATUS_REBOOT_REQUIRED)
        throw std::runtime_error(tr(Msg::RtDriverUnregister, {std::to_string(status)}));
    if (std::filesystem::exists(destination) && !DeleteFileW(destination.c_str())) {
        if (!MoveFileExW(destination.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) win_error(Msg::RtDriverRemove);
        log_info(tr(Msg::RtDriverUnregisteredFilePending));
    } else log_info(status == NF_STATUS_REBOOT_REQUIRED ? tr(Msg::RtDriverUnregisteredReboot) : tr(Msg::RtDriverRemoved));
}
}
