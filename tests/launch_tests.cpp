#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include "launch.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template <typename Function> void rejects(Function function, const char* message) {
    ++checks;
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
std::vector<std::wstring> split(const std::wstring& command) {
    int count = 0;
    auto* raw = CommandLineToArgvW(command.c_str(), &count);
    if (!raw) throw std::runtime_error("CommandLineToArgvW failed");
    std::vector<std::wstring> result(raw, raw + count);
    LocalFree(raw);
    return result;
}
void parsing() {
    const std::filesystem::path exe = L"C:\\程序 目录\\ProcSocks.exe";
    const std::filesystem::path cwd = L"C:\\调用目录\\sub";
    const auto parse = [&](std::vector<std::wstring> args, bool interactive = true) {
        return ps::parse_launch(args, interactive, exe, cwd);
    };
    auto result = parse({});
    check(result.command == ps::Command::Tui && !result.explicitConfig && result.configPath == L"C:\\程序 目录\\config.json", "interactive no-arg defaults beside exe");
    result = parse({}, false);
    check(result.command == ps::Command::Help && result.helpExitCode == 2, "redirected no-arg usage exit2");
    result = parse({L"--help", L"ignored"}, false);
    check(result.command == ps::Command::Help && result.helpExitCode == 0, "help legacy behavior");
    check(parse({L"-h"}).command == ps::Command::Help, "help alias");
    check(parse({L"--version", L"ignored"}, false).command == ps::Command::Version, "version legacy behavior");
    result = parse({L"unknown", L"--config"});
    check(result.command == ps::Command::Invalid && result.helpExitCode == 2, "unknown command usage exit2");
    for (const auto command : {L"check", L"run"}) {
        result = parse({command}, false);
        check(result.configPath == L"config.json" && !result.explicitConfig, "CLI default remains relative to caller cwd");
        result = parse({command, L"--config", L"..\\custom.json"}, false);
        check(result.configPath == L"..\\custom.json" && result.explicitConfig, "CLI explicit relative path preserved");
        rejects([&] { parse({command, L"--config"}); }, "missing config argument accepted");
        rejects([&] { parse({command, L"--config", L""}); }, "empty config path accepted");
        rejects([&] { parse({command, L"--other", L"x"}); }, "unknown option accepted");
        rejects([&] { parse({command, L"--config", L"x", L"extra"}); }, "trailing config argument accepted");
    }
    check(parse({L"check"}).command == ps::Command::Check, "check parsed");
    check(parse({L"run"}).command == ps::Command::Run, "run parsed");
    check(parse({L"install-driver"}).command == ps::Command::Install, "install parsed");
    check(parse({L"uninstall-driver"}).command == ps::Command::Uninstall, "uninstall parsed");
    rejects([&] { parse({L"install-driver", L"--config", L"x"}); }, "install config option accepted");
    rejects([&] { parse({L"uninstall-driver", L"extra"}); }, "uninstall extra accepted");
    result = parse({L"tui"});
    check(result.command == ps::Command::Tui && !result.explicitConfig && result.configPath == L"C:\\程序 目录\\config.json", "explicit TUI command uses exe default");
    result = parse({L"tui", L"--config", L"..\\配置\\profile.json"});
    check(result.explicitConfig && result.configPath == L"C:\\调用目录\\配置\\profile.json" && result.configPath.is_absolute(), "TUI relative config normalized against caller cwd");
    result = parse({L"tui", L"--config", L"D:\\配置\\..\\profile.json"});
    check(result.configPath == L"D:\\profile.json", "absolute config normalized");
    result = parse({L"tui", L"--config", L"\\settings.json"});
    check(result.configPath == L"C:\\settings.json", "root-relative config uses caller drive");
    result = parse({L"tui", L"--config", L"c:settings.json"});
    check(result.configPath == L"C:\\调用目录\\sub\\settings.json", "same-drive relative config uses caller cwd");
    result = parse({L"tui", L"--config", L"\\\\server\\share\\settings.json"});
    check(result.configPath.native() == L"\\\\server\\share\\settings.json", "UNC config preserved");
    result = ps::parse_launch({}, true, L"\\\\server\\share\\app\\ProcSocks.exe", cwd);
    check(result.configPath.native() == L"\\\\server\\share\\app\\config.json", "UNC executable default preserved");
    result = ps::parse_launch({L"tui", L"--config", L"..\\settings.json"}, true, exe, L"\\\\server\\share\\sub");
    check(result.configPath.native() == L"\\\\server\\share\\settings.json", "relative config anchored to UNC cwd");
    rejects([&] { parse({L"tui", L"--config", L"\\\\server"}); }, "incomplete UNC path accepted");
    rejects([&] { parse({L"tui", L"--config", L"D:settings.json"}); }, "ambiguous different-drive relative config accepted");
    result = parse({L"tui"}, false);
    check(result.command == ps::Command::Tui && !result.explicitConfig && result.configPath == L"C:\\程序 目录\\config.json",
          "non-interactive explicit TUI must resolve default config before language-aware entry rejection");
    result = parse({L"tui", L"--config", L"..\\配置\\profile.json"}, false);
    check(result.command == ps::Command::Tui && result.explicitConfig && result.configPath == L"C:\\调用目录\\配置\\profile.json",
          "non-interactive TUI must resolve supplied config before language-aware entry rejection");
    rejects([&] { parse({L"tui", L"--config"}); }, "malformed TUI config accepted");
    result = parse({L"tui", L"--ui-language", L"zh-CN"});
    check(result.uiLanguage == ps::Language::ZhCN && !result.explicitConfig, "TUI language context lost default config semantics");
    result = parse({L"tui", L"--ui-language", L"en-US", L"--config", L"..\\配置\\profile.json"});
    check(result.uiLanguage == ps::Language::EnUS && result.explicitConfig, "TUI language context before config was rejected");
    rejects([&] { parse({L"tui", L"--ui-language"}); }, "missing internal language accepted");
    rejects([&] { parse({L"tui", L"--ui-language", L"auto"}); }, "unresolved internal language accepted");
    rejects([&] { parse({L"tui", L"--ui-language", L"zh-CN", L"--ui-language", L"en-US"}); }, "duplicate internal language accepted");
    rejects([&] { parse({L"tui", L"--config", L"a", L"--config", L"b"}); }, "duplicate config accepted");
    for (const auto command : {L"run", L"check", L"install-driver", L"uninstall-driver"})
        rejects([&] { parse({command, L"--ui-language", L"en-US"}); }, "internal TUI language changed CLI grammar");
}
void quoting() {
    const std::vector<std::wstring> values{
        L"", L"simple", L"a b\tc", L"C:\\folder\\", L"C:\\路径 with spaces\\配置.json",
        L"quote\"inside", L"before\\\"after", L"many\\\\\"quotes\\\\", L"&|<>^%$(literal)", L"\r\n"
    };
    for (const auto& value : values) {
        auto parsed = split(L"ProcSocks.exe " + ps::quote_windows_argument(value));
        check(parsed.size() == 2 && parsed[1] == value, "Windows argument quoting round trip");
    }
    rejects([] { ps::quote_windows_argument(std::wstring(L"a\0b", 3)); }, "NUL command argument accepted");
    const std::filesystem::path config = L"C:\\路径 with spaces\\配置.json";
    auto explicitArgs = split(L"ProcSocks.exe " + ps::tui_elevation_arguments(config, true));
    check(explicitArgs == std::vector<std::wstring>{L"ProcSocks.exe", L"tui", L"--config", config.wstring(), L"--ui-language", L"en-US"}, "elevation forwards one exact absolute config argument");
    auto defaultArgs = split(L"ProcSocks.exe " + ps::tui_elevation_arguments(config, false));
    check(defaultArgs == std::vector<std::wstring>{L"ProcSocks.exe", L"tui", L"--ui-language", L"en-US"}, "default elevation preserves implicit config status");
    rejects([] { ps::tui_elevation_arguments(L"relative.json", true); }, "relative elevation config accepted");
    for (const auto language : {ps::Language::ZhCN, ps::Language::EnUS}) {
        for (const auto explicitConfig : {false, true}) {
            const auto command = ps::tui_elevation_arguments(config, explicitConfig, language);
            auto forwarded = split(L"ProcSocks.exe " + command);
            forwarded.erase(forwarded.begin());
            // Simulate an administrator using a different UI language. The
            // forwarded resolved context must survive independently of it.
            ps::set_language(language == ps::Language::ZhCN ? ps::Language::EnUS : ps::Language::ZhCN);
            const auto elevated = ps::parse_launch(forwarded, true, L"C:\\程序 目录\\ProcSocks.exe", L"C:\\Windows\\System32");
            check(elevated.uiLanguage == language && elevated.explicitConfig == explicitConfig,
                  "elevation lost caller language or implicit config status");
            check(!explicitConfig || elevated.configPath == config, "elevation changed Chinese/spaced config path");
        }
    }
    ps::set_language(ps::Language::EnUS);
}
void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    if (!stream) throw std::runtime_error("Cannot write temporary fixture");
}
std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read temporary fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void copying() {
    const auto fixture = ps::executable_path().parent_path() /
        (L"launch-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(fixture.c_str(), nullptr)) throw std::runtime_error("Cannot create isolated launch test fixture");
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            // Fixed, known files in the test-owned directory only; never recurse.
            DeleteFileW((directory / L"example.json").c_str());
            DeleteFileW((directory / L"config.json").c_str());
            RemoveDirectoryW(directory.c_str());
        }
    } cleanup{fixture};
    const auto example = fixture / L"example.json";
    const auto config = fixture / L"config.json";
    write_file(example, "{\"fixture\":1}\n");
    check(ps::ensure_default_config(config, example), "missing default config created");
    check(read_file(config) == read_file(example), "default contents copied exactly");
    write_file(config, "{\"user_setting\":true}\n");
    write_file(example, "{\"fixture\":2}\n");
    check(!ps::ensure_default_config(config, example), "existing default not overwritten");
    check(read_file(config) == "{\"user_setting\":true}\n", "user edits preserved");
    check(!ps::ensure_default_config(config, fixture / L"missing.json"), "existing config independent of example availability");
    rejects([&] { ps::ensure_default_config(fixture / L"absent.json", fixture / L"missing.json"); }, "missing example accepted");
    check(!std::filesystem::exists(fixture / L"absent.json"), "failed copy creates no config");
    check(!ps::ensure_default_config(fixture, example), "existing directory never overwritten");
}
}
int main() {
    ps::set_language(ps::Language::EnUS);
    try {
        parsing(); quoting(); copying();
        check(ps::executable_path().is_absolute(), "actual executable path is absolute");
        std::cout << "launch_tests: " << checks << " checks passed; no UAC or editor launched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "launch_tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
