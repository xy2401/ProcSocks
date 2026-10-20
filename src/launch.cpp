#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include "launch.hpp"
#include <algorithm>
#include <stdexcept>

namespace ps {
namespace {
[[noreturn]] void launch_error(Msg operation, DWORD error = GetLastError()) {
    throw std::runtime_error(tr(Msg::LaunchWindowsError, {tr(operation), std::to_string(error)}));
}
void check_path(const std::filesystem::path& path) {
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos)
        throw std::runtime_error(tr(Msg::PathRequired));
}
bool drive_letter(wchar_t value) { return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z'); }
std::wstring windows_path(const std::filesystem::path& path) {
    auto result = path.wstring();
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
}
bool windows_absolute(const std::wstring& path) {
    if (path.size() >= 3 && drive_letter(path[0]) && path[1] == L':' && path[2] == L'\\') return true;
    if (path.size() <= 4 || path[0] != L'\\' || path[1] != L'\\') return false;
    const auto serverEnd = path.find(L'\\', 2);
    return serverEnd != std::wstring::npos && serverEnd > 2 && serverEnd + 1 < path.size() && path[serverEnd + 1] != L'\\';
}
void require_absolute(const std::filesystem::path& path) {
    check_path(path);
    if (!windows_absolute(windows_path(path))) throw std::runtime_error(tr(Msg::AbsolutePathRequired));
}
std::filesystem::path normalize_absolute(const std::filesystem::path& path) {
    require_absolute(path);
    const auto text = windows_path(path);
    // MinGW's filesystem path parser loses the UNC root during lexical
    // normalization. Win32 preserves it and never consults the cwd here because
    // callers supply an already anchored absolute path.
    std::vector<wchar_t> buffer(32768);
    const auto length = GetFullPathNameW(text.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size()) launch_error(Msg::NormalizeConfigPathFailed);
    return std::filesystem::path(std::wstring(buffer.data(), length));
}
std::filesystem::path tui_config_path(const std::filesystem::path& input, const std::filesystem::path& cwd) {
    check_path(input);
    require_absolute(cwd);
    const auto text = windows_path(input);
    const auto base = windows_path(cwd);
    if (windows_absolute(text)) return normalize_absolute(text);
    if (text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\') throw std::runtime_error(tr(Msg::UncShareRequired));
    if (text.size() >= 2 && drive_letter(text[0]) && text[1] == L':') {
        // Drive-relative paths on the current drive have an unambiguous base.
        // A different drive would depend on hidden per-drive current-directory
        // state that does not survive elevation; require an explicit absolute path.
        if (base.size() < 2 || CompareStringOrdinal(text.c_str(), 2, base.c_str(), 2, TRUE) != CSTR_EQUAL)
            throw std::runtime_error(tr(Msg::OtherDriveAbsoluteRequired));
        return normalize_absolute(base + L"\\" + text.substr(2));
    }
    if (text.front() == L'\\') {
        if (base.size() >= 2 && drive_letter(base[0]) && base[1] == L':') return normalize_absolute(base.substr(0, 2) + text);
        const auto serverEnd = base.find(L'\\', 2);
        const auto shareEnd = base.find(L'\\', serverEnd + 1);
        return normalize_absolute(base.substr(0, shareEnd) + text);
    }
    return normalize_absolute(base + L"\\" + text);
}
struct NativeHandle {
    HANDLE value = nullptr;
    ~NativeHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ComApartment {
    bool owned = false;
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        owned = SUCCEEDED(result);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            throw std::runtime_error(tr(Msg::ShellComFailed, {std::to_string(static_cast<unsigned long>(result))}));
    }
    ~ComApartment() { if (owned) CoUninitialize(); }
};
}

LaunchOptions parse_launch(const std::vector<std::wstring>& args, bool interactive,
                           const std::filesystem::path& exePath, const std::filesystem::path& callerCwd) {
    LaunchOptions options;
    if (args.empty()) {
        if (!interactive) { options.command = Command::Help; options.helpExitCode = 2; return options; }
        require_absolute(exePath);
        options.command = Command::Tui;
        options.configPath = normalize_absolute(exePath.parent_path() / L"config.json");
        return options;
    }
    const auto& command = args.front();
    // Preserve existing --help/--version behavior, including ignored trailing args.
    if (command == L"--help" || command == L"-h") { options.command = Command::Help; return options; }
    if (command == L"--version") { options.command = Command::Version; return options; }
    if (command == L"check") options.command = Command::Check;
    else if (command == L"run") options.command = Command::Run;
    else if (command == L"install-driver") options.command = Command::Install;
    else if (command == L"uninstall-driver") options.command = Command::Uninstall;
    else if (command == L"tui") options.command = Command::Tui;
    else { options.command = Command::Invalid; options.helpExitCode = 2; return options; }
    if (options.command == Command::Install || options.command == Command::Uninstall) {
        if (args.size() != 1) throw std::runtime_error(tr(Msg::DriverConfigArgumentsRejected));
        return options;
    }
    if (options.command == Command::Tui) {
        // Only the TUI accepts the internal resolved-language context. This also
        // preserves the caller's language if UAC uses a different admin account.
        for (std::size_t index = 1; index < args.size(); index += 2) {
            if (args[index] == L"--ui-language") {
                if (index + 1 == args.size() || options.uiLanguage != Language::Auto ||
                    (args[index + 1] != L"zh-CN" && args[index + 1] != L"en-US"))
                    throw std::runtime_error(tr(Msg::UiLanguageArgumentInvalid));
                options.uiLanguage = args[index + 1] == L"zh-CN" ? Language::ZhCN : Language::EnUS;
            } else if (args[index] == L"--config" && index + 1 < args.size() && !options.explicitConfig) {
                options.configPath = args[index + 1];
                check_path(options.configPath);
                options.explicitConfig = true;
            } else throw std::runtime_error(tr(Msg::ConfigArgumentExpected));
        }
    } else if (args.size() == 3 && args[1] == L"--config") {
        options.configPath = args[2];
        check_path(options.configPath);
        options.explicitConfig = true;
    } else if (args.size() != 1) throw std::runtime_error(tr(Msg::ConfigArgumentExpected));
    if (options.command == Command::Tui) {
        require_absolute(exePath);
        options.configPath = options.explicitConfig ? tui_config_path(options.configPath, callerCwd)
            : normalize_absolute(exePath.parent_path() / L"config.json");
    } else if (!options.explicitConfig) options.configPath = L"config.json";
    return options;
}

std::filesystem::path executable_path() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) launch_error(Msg::ExecutablePathFailed);
    return normalize_absolute(std::filesystem::path(std::wstring(buffer.data(), length)));
}

bool interactive_console() {
    DWORD inputMode = 0, outputMode = 0;
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    return input && input != INVALID_HANDLE_VALUE && output && output != INVALID_HANDLE_VALUE &&
           GetConsoleMode(input, &inputMode) && GetConsoleMode(output, &outputMode);
}

std::wstring quote_windows_argument(const std::wstring& argument) {
    if (argument.find(L'\0') != std::wstring::npos) throw std::runtime_error(tr(Msg::ArgumentNulRejected));
    // Always quote, following the Windows CRT/CommandLineToArgvW backslash rule.
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(character);
        }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring tui_elevation_arguments(const std::filesystem::path& absoluteConfig, bool explicitConfig, Language language) {
    require_absolute(absoluteConfig);
    if (language == Language::Auto) language = current_language();
    const auto arguments = explicitConfig ? L"tui --config " + quote_windows_argument(absoluteConfig.wstring()) : L"tui";
    return arguments + std::wstring(L" --ui-language ") + unicode_text(language_tag(language));
}

ElevationResult elevate_tui(const std::filesystem::path& exe, const std::filesystem::path& absoluteConfig, bool explicitConfig, Language language) {
    require_absolute(exe);
    require_absolute(absoluteConfig);
    const auto parameters = tui_elevation_arguments(absoluteConfig, explicitConfig, language);
    const auto directory = exe.parent_path().wstring();
    const auto executable = exe.wstring();
    ComApartment apartment;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const auto error = GetLastError();
        if (error == ERROR_CANCELLED) return ElevationResult::Cancelled;
        launch_error(Msg::ElevationFailed, error);
    }
    NativeHandle process{info.hProcess};
    return ElevationResult::Started;
}

bool ensure_default_config(const std::filesystem::path& config, const std::filesystem::path& example) {
    check_path(config); check_path(example);
    if (GetFileAttributesW(config.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    auto error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) launch_error(Msg::InspectDefaultFailed, error);
    // CopyFile's fail-if-exists flag also protects a config created concurrently
    // between the existence check and this copy. Existing contents are untouched.
    if (CopyFileW(example.c_str(), config.c_str(), TRUE)) return true;
    error = GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) return false;
    launch_error(Msg::CreateDefaultFailed, error);
}

void edit_config(const std::filesystem::path& config) {
    require_absolute(config);
    std::vector<wchar_t> systemDirectory(32768);
    const auto length = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (!length || length >= systemDirectory.size()) launch_error(Msg::SystemDirectoryFailed);
    const auto editor = std::filesystem::path(std::wstring(systemDirectory.data(), length)) / L"notepad.exe";
    const auto command = quote_windows_argument(editor.wstring()) + L" " + quote_windows_argument(config.wstring());
    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION info{};
    const auto directory = config.parent_path().wstring();
    if (!CreateProcessW(editor.c_str(), writable.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.c_str(), &startup, &info)) launch_error(Msg::NotepadOpenFailed);
    NativeHandle process{info.hProcess};
    NativeHandle thread{info.hThread};
}
}
