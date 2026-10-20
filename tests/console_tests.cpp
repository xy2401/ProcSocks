// Include the UI translation unit to exercise its actual private Console and
// session pump. The test build must exclude the separate tui.o object.
#include "../src/tui.cpp"
#include <shellapi.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
int checks = 0;
void check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void inject_key(HANDLE input, WORD key, wchar_t character = 0) {
    INPUT_RECORD records[2]{};
    for (auto& record : records) {
        record.EventType = KEY_EVENT;
        record.Event.KeyEvent.wRepeatCount = 1;
        record.Event.KeyEvent.wVirtualKeyCode = key;
        record.Event.KeyEvent.uChar.UnicodeChar = character;
    }
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[1].Event.KeyEvent.bKeyDown = FALSE;
    DWORD written = 0;
    if (!WriteConsoleInputW(input, records, 2, &written) || written != 2)
        throw std::runtime_error("WriteConsoleInputW failed");
}
KEY_EVENT_RECORD read_key(const ps::Console& console) {
    const auto deadline = GetTickCount64() + 1500;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(console.input(), 100) != WAIT_OBJECT_0) continue;
        const auto record = console.read();
        if (record && record->EventType == KEY_EVENT && record->Event.KeyEvent.bKeyDown)
            return record->Event.KeyEvent;
    }
    throw std::runtime_error("Console did not deliver the injected key");
}
struct BackendState {
    std::vector<std::string> calls;
    std::atomic_bool destroyed{false};
    std::thread::id thread;
};
class FakeBackend final : public ps::session_testing::Backend {
public:
    explicit FakeBackend(BackendState& state) : state_(state) {
        state_.thread = std::this_thread::get_id();
        state_.calls.emplace_back("construct");
    }
    ~FakeBackend() override { state_.calls.emplace_back("destroy"); state_.destroyed = true; }
    void start_tracker() override { state_.calls.emplace_back("tracker-start"); }
    void start_engine() override { state_.calls.emplace_back("engine-start"); }
    void stop_engine() override { state_.calls.emplace_back("engine-stop"); std::this_thread::sleep_for(25ms); }
    void stop_tracker() override { state_.calls.emplace_back("tracker-stop"); }
private:
    BackendState& state_;
};

void native_console_test() {
    check(ps::interactive_console(), "Hidden child lacks a real native console");
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD originalInput = 0, originalOutput = 0;
    check(GetConsoleMode(input, &originalInput) != FALSE, "Cannot read baseline input mode");
    check(GetConsoleMode(output, &originalOutput) != FALSE, "Cannot read baseline output mode");
    const UINT originalCodePage = GetConsoleOutputCP();
    const DWORD baselineInput = originalInput | ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
    check(SetConsoleMode(input, baselineInput) != FALSE, "Cannot establish Quick Edit baseline");
    check(SetConsoleOutputCP(437) != FALSE, "Cannot establish output code-page baseline");
    struct Restore {
        HANDLE input, output;
        DWORD inputMode, outputMode;
        UINT codePage;
        ~Restore() { SetConsoleMode(input, inputMode); SetConsoleMode(output, outputMode); SetConsoleOutputCP(codePage); }
    } restore{input, output, originalInput, originalOutput, originalCodePage};
    {
        ps::Console console;
        DWORD mode = 0;
        check(GetConsoleMode(input, &mode) != FALSE, "Cannot read TUI input mode");
        check(!(mode & ENABLE_QUICK_EDIT_MODE), "Quick Edit remains enabled");
        check(!(mode & (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT)), "TUI did not disable line/echo/mouse input");
        check((mode & ENABLE_PROCESSED_INPUT) && (mode & ENABLE_WINDOW_INPUT), "TUI processed/window input missing");
        check(GetConsoleOutputCP() == CP_UTF8, "TUI UTF-8 output mode missing");
        // Read each glyph with room for both screen cells: clipping a wide CJK
        // glyph to one cell makes ReadConsoleOutputCharacterW return a space.
        // Position glyphs independently because font widths may differ.
        const std::wstring glyphs = L"中文菜单Ω";
        for (std::size_t index = 0; index < glyphs.size(); ++index) {
            const COORD where{static_cast<SHORT>(index * 4), 0};
            check(SetConsoleCursorPosition(output, where) != FALSE, "Cannot position Unicode test cursor");
            console.write(std::wstring(1, glyphs[index]));
            wchar_t observed[2]{};
            DWORD read = 0;
            const bool readOk = ReadConsoleOutputCharacterW(output, observed, 2, where, &read) != FALSE;
            const auto diagnostic = "WriteConsole Unicode output did not round-trip through screen buffer: index=" + std::to_string(index) +
                " expected=" + std::to_string(static_cast<unsigned>(glyphs[index])) + " observed=" + std::to_string(static_cast<unsigned>(observed[0])) +
                " read=" + std::to_string(read) + " ok=" + std::to_string(readOk);
            check(readOk && read >= 1 && observed[0] == glyphs[index], diagnostic.c_str());
        }
        // A localized runtime error must use Unicode on stderr too, independent
        // of the active OEM code page used by legacy narrow console writes.
        const auto errorOutput = GetStdHandle(STD_ERROR_HANDLE);
        DWORD errorMode = 0;
        check(GetConsoleMode(errorOutput, &errorMode) != FALSE, "Child stderr is not a native console");
        check(SetConsoleOutputCP(437) != FALSE, "Cannot set OEM code page for Unicode log test");
        check(SetConsoleCursorPosition(errorOutput, COORD{0, 2}) != FALSE, "Cannot position log test cursor");
        ps::log_error(ps::tr(ps::Msg::AdminRequired));
        wchar_t logGlyph[2]{};
        DWORD logRead = 0;
        check(ReadConsoleOutputCharacterW(errorOutput, logGlyph, 2, COORD{28, 2}, &logRead) && logRead >= 1 && logGlyph[0] == L'需',
              "Localized stderr log did not preserve Chinese Unicode under OEM code page");
        check(SetConsoleOutputCP(CP_UTF8) != FALSE, "Cannot restore TUI code page after log test");
        check(FlushConsoleInputBuffer(input) != FALSE, "Cannot clear console input before key tests");
        std::size_t selected = 0;
        const WORD keys[]{VK_DOWN, VK_DOWN, VK_UP, VK_RETURN, VK_ESCAPE};
        for (auto key : keys) {
            inject_key(input, key, key == VK_RETURN ? L'\r' : 0);
            const auto record = read_key(console);
            check(record.wVirtualKeyCode == key, "Console key changed during native round trip");
            const auto action = ps::menu_key(selected, record.wVirtualKeyCode, record.uChar.UnicodeChar);
            if (key == VK_RETURN) check(action && *action == ps::MenuAction::Check, "Arrows/Enter did not select expected menu item");
            if (key == VK_ESCAPE) check(action && *action == ps::MenuAction::Exit, "Native Esc did not select Exit");
        }
        check(FlushConsoleInputBuffer(input) != FALSE, "Cannot clear input before session pump");
        ps::ControlSignals signals;
        check(signals.begin_operation(), "Cannot begin native console session");
        BackendState backend;
        std::atomic_bool injected{false};
        ps::session_testing::run_with_backend(signals, [&] { return std::make_unique<FakeBackend>(backend); },
            [&](HANDLE completed, HANDLE ready) {
                std::exception_ptr injectionError;
                std::thread injector([&] {
                    try {
                        if (WaitForSingleObject(ready, 3000) != WAIT_OBJECT_0) throw std::runtime_error("Fake session did not become ready");
                        inject_key(input, VK_ESCAPE);
                        injected = true;
                    } catch (...) { injectionError = std::current_exception(); signals.request_stop(); }
                });
                try { ps::pump_session(console, signals, completed, ready); }
                catch (...) { signals.request_stop(); injector.join(); throw; }
                injector.join();
                if (injectionError) std::rethrow_exception(injectionError);
                check(WaitForSingleObject(completed, 0) == WAIT_OBJECT_0, "Native pump returned before session completion");
                check(backend.destroyed, "Native pump returned before backend destruction");
            });
        check(injected, "Ready-triggered Esc was not injected");
        check(signals.stop_requested() && !signals.exit_requested(), "Esc should stop a session without exiting the menu");
        check(backend.thread != std::this_thread::get_id(), "Session backend ran on console thread");
        check(backend.calls == std::vector<std::string>{"construct", "tracker-start", "engine-start", "engine-stop", "tracker-stop", "destroy"},
              "Native session pump cleanup order is wrong");
        signals.end_operation();
        check(!signals.stop_requested() && !signals.exit_requested(), "Session cancellation was not retired for menu reuse");
    }
    DWORD restoredInput = 0, restoredOutput = 0;
    check(GetConsoleMode(input, &restoredInput) && restoredInput == baselineInput, "Console destructor did not restore input/Quick Edit mode");
    check(GetConsoleMode(output, &restoredOutput) && restoredOutput == originalOutput, "Console destructor did not restore output mode");
    check(GetConsoleOutputCP() == 437, "Console destructor did not restore output code page");
}

int child(const std::filesystem::path& diagnostic) {
    try {
        native_console_test();
        std::ofstream report(diagnostic, std::ios::binary | std::ios::trunc);
        report << "console_tests: " << checks << " native-console checks passed; no UAC, driver, ETW, or editor invoked\n";
        return report ? 0 : 2;
    } catch (const std::exception& error) {
        std::ofstream report(diagnostic, std::ios::binary | std::ios::trunc);
        report << "console_tests child FAILED: " << error.what() << '\n';
        return 1;
    }
}
int parent() {
    const auto exe = ps::executable_path();
    const auto diagnostic = exe.parent_path() /
        (L"console-smoke-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".txt");
    {
        Handle file{CreateFileW(diagnostic.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create build-local console diagnostic file");
    }
    struct Cleanup { std::filesystem::path path; ~Cleanup() { DeleteFileW(path.c_str()); } } cleanup{diagnostic};
    auto command = ps::quote_windows_argument(exe.wstring()) + L" --native-console-child " + ps::quote_windows_argument(diagnostic.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    const auto directory = exe.parent_path().wstring();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        directory.c_str(), &startup, &info))
        throw std::runtime_error("Cannot create hidden native-console child (Windows error " + std::to_string(GetLastError()) + ")");
    Handle process{info.hProcess}, thread{info.hThread};
    const auto wait = WaitForSingleObject(process.value, 15000);
    if (wait != WAIT_OBJECT_0) {
        // Terminate only this test's own child, identified by the returned handle.
        TerminateProcess(process.value, 97);
        WaitForSingleObject(process.value, 2000);
        throw std::runtime_error(wait == WAIT_TIMEOUT ? "Native-console child exceeded 15-second limit" : "Cannot wait for native-console child");
    }
    DWORD result = 0;
    if (!GetExitCodeProcess(process.value, &result)) throw std::runtime_error("Cannot read console child exit status");
    std::ifstream file(diagnostic, std::ios::binary);
    const std::string report{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (result != 0) throw std::runtime_error(report.empty() ? "Console child failed without diagnostic; exit=" + std::to_string(result) : report);
    if (report.empty()) throw std::runtime_error("Console child omitted its test report");
    std::cout << report;
    return 0;
}
}

int main() {
    ps::set_language(ps::Language::ZhCN);
    try {
        int count = 0;
        auto* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments) throw std::runtime_error("Cannot parse console-test command line");
        std::vector<std::wstring> args(arguments, arguments + count);
        LocalFree(arguments);
        if (args.size() == 3 && args[1] == L"--native-console-child") return child(args[2]);
        if (args.size() != 1) throw std::runtime_error("Unexpected console-test arguments");
        return parent();
    } catch (const std::exception& error) {
        std::cerr << "console_tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
