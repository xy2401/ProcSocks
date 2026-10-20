#include "net.hpp"
#include "tui.hpp"
#include "controller.hpp"
#include "log.hpp"
#include "session.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace ps {
std::optional<MenuAction> menu_key(std::size_t& selected, unsigned short key, wchar_t character) {
    selected %= 6;
    if (key == VK_UP) selected = (selected + 5) % 6;
    else if (key == VK_DOWN) selected = (selected + 1) % 6;
    else if (key == VK_RETURN) return static_cast<MenuAction>(selected);
    else if (key == VK_ESCAPE) return MenuAction::Exit;
    else if (character >= L'1' && character <= L'6') {
        selected = static_cast<std::size_t>(character - L'1');
        return static_cast<MenuAction>(selected);
    }
    return {};
}
std::wstring config_summary(const Config& config) {
    auto host = from_utf8(config.proxy.host);
    if (host.find(L':') != std::wstring::npos) host = L"[" + host + L"]";
    return trw(Msg::ConfigSummary, {to_utf8(host), std::to_string(config.proxy.port),
        tr(config.proxy.authenticate ? Msg::AuthConfigured : Msg::AuthNone), std::to_string(config.processes.size()),
        tr(config.includeChildren ? Msg::FeatureOn : Msg::FeatureOff),
        tr(config.bypassPrivateNetworks ? Msg::FeatureOn : Msg::FeatureOff)});
}
void perform_menu_action(MenuAction action, const MenuServices& s) {
    if (s.cancelled()) return;
    switch (action) {
    case MenuAction::Start: {
        const auto config = s.readConfig();
        if (s.cancelled()) return;
        if (s.driverState() == DriverState::NotInstalled) {
            if (!s.confirm(trw(Msg::InstallAndStartPrompt))) return;
            if (s.cancelled()) return;
            s.installDriver();
        }
        if (s.cancelled()) return;
        s.startDriver();
        if (!s.cancelled()) s.runSession(config);
        break;
    }
    case MenuAction::Check:
        s.tell(trw(Msg::ConfigCheckPassed) + config_summary(s.readConfig()));
        break;
    case MenuAction::Edit:
        s.editConfig();
        s.tell(trw(Msg::EditorOpened));
        break;
    case MenuAction::Install:
        s.installDriver();
        s.tell(trw(Msg::InstallFinished));
        break;
    case MenuAction::Uninstall:
        if (s.confirm(trw(Msg::UninstallPrompt)) && !s.cancelled()) {
            s.uninstallDriver();
            s.tell(trw(Msg::UninstallFinished));
        }
        break;
    case MenuAction::Exit: break;
    }
}

namespace {
class Console {
public:
    Console() {
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!GetConsoleMode(input_, &inputMode_) || !GetConsoleMode(output_, &outputMode_))
            throw std::runtime_error(tr(Msg::TuiInteractiveRequired));
        codePage_ = GetConsoleOutputCP();
        const DWORD mode = (inputMode_ | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT) &
            ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
        if (!SetConsoleMode(input_, mode)) throw std::runtime_error(tr(Msg::ConsoleInputConfigFailed));
        // Native console messages and logs use WriteConsoleW; retain the console mode contract.
        if (!SetConsoleOutputCP(CP_UTF8) || !SetConsoleMode(output_, outputMode_ | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT)) {
            SetConsoleMode(input_, inputMode_);
            SetConsoleOutputCP(codePage_);
            throw std::runtime_error(tr(Msg::ConsoleOutputConfigFailed));
        }
    }
    ~Console() {
        SetConsoleMode(input_, inputMode_);
        SetConsoleMode(output_, outputMode_);
        SetConsoleOutputCP(codePage_);
    }
    HANDLE input() const { return input_; }
    void write(const std::wstring& value) const {
        write_console_message(value);
    }
    void clear() const {
        // Called only in idle state, after session workers have all joined.
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output_, &info)) return;
        const COORD origin{0, info.srWindow.Top};
        const DWORD count = static_cast<DWORD>(info.dwSize.X) * (info.srWindow.Bottom - info.srWindow.Top + 1);
        DWORD written = 0;
        FillConsoleOutputCharacterW(output_, L' ', count, origin, &written);
        FillConsoleOutputAttribute(output_, info.wAttributes, count, origin, &written);
        SetConsoleCursorPosition(output_, origin);
    }
    std::optional<INPUT_RECORD> read() const {
        INPUT_RECORD record{};
        DWORD count = 0;
        if (!ReadConsoleInputW(input_, &record, 1, &count)) throw std::runtime_error(tr(Msg::ConsoleReadFailed));
        if (count) return record;
        return {};
    }
    bool wait_key(ControlSignals& signals) const {
        const HANDLE handles[]{signals.stop_event(), input_};
        while (!signals.exit_requested()) {
            const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) return false;
            if (result != WAIT_OBJECT_0 + 1) throw std::runtime_error(tr(Msg::ConsoleWaitFailed));
            const auto record = read();
            if (record && record->EventType == KEY_EVENT && record->Event.KeyEvent.bKeyDown) return true;
        }
        return false;
    }
    bool poll_cancel(ControlSignals& signals) const {
        while (WaitForSingleObject(input_, 0) == WAIT_OBJECT_0) {
            const auto record = read();
            if (record && record->EventType == KEY_EVENT && record->Event.KeyEvent.bKeyDown &&
                record->Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) signals.request_stop();
        }
        return signals.stop_requested() || signals.exit_requested();
    }
private:
    HANDLE input_ = INVALID_HANDLE_VALUE, output_ = INVALID_HANDLE_VALUE;
    DWORD inputMode_ = 0, outputMode_ = 0;
    UINT codePage_ = 0;
};
std::wstring driver_label(DriverState state) {
    switch (state) {
    case DriverState::NotInstalled: return trw(Msg::DriverAbsentLabel);
    case DriverState::Stopped: return trw(Msg::DriverStoppedLabel);
    case DriverState::Running: return trw(Msg::DriverRunningLabel);
    case DriverState::Pending: return trw(Msg::DriverPendingLabel);
    }
    return trw(Msg::UnknownLabel);
}
bool confirm(const Console& console, ControlSignals& signals, const std::wstring& prompt) {
    console.write(L"\n" + prompt + trw(Msg::ConfirmationChoices));
    const HANDLE handles[]{signals.stop_event(), console.input()};
    for (;;) {
        const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) return false;
        if (result != WAIT_OBJECT_0 + 1) throw std::runtime_error(tr(Msg::ConsoleWaitFailed));
        const auto record = console.read();
        if (!record || record->EventType != KEY_EVENT || !record->Event.KeyEvent.bKeyDown) continue;
        const auto& key = record->Event.KeyEvent;
        if (key.uChar.UnicodeChar == L'y' || key.uChar.UnicodeChar == L'Y') { console.write(L"Y\n"); return true; }
        if (key.uChar.UnicodeChar == L'n' || key.uChar.UnicodeChar == L'N' || key.wVirtualKeyCode == VK_ESCAPE || key.wVirtualKeyCode == VK_RETURN) {
            console.write(trw(Msg::UiCancelled)); return false;
        }
    }
}
void pump_session(const Console& console, ControlSignals& signals, HANDLE completed, HANDLE ready) {
    bool announced = false, cancelling = false;
    for (;;) {
        if (WaitForSingleObject(completed, 0) == WAIT_OBJECT_0) return;
        if (signals.stop_requested()) {
            if (!cancelling) { console.write(trw(Msg::ProxyStopping)); cancelling = true; }
            if (WaitForSingleObject(completed, 50) == WAIT_FAILED) throw std::runtime_error(tr(Msg::UiSessionWaitFailed));
            continue;
        }
        if (!announced && WaitForSingleObject(ready, 0) == WAIT_OBJECT_0) {
            console.write(trw(Msg::ProxyStartedUi));
            announced = true;
        }
        const HANDLE handles[]{completed, signals.stop_event(), console.input()};
        const auto result = WaitForMultipleObjects(3, handles, FALSE, 50);
        if (result == WAIT_OBJECT_0) return;
        if (result == WAIT_OBJECT_0 + 2) console.poll_cancel(signals);
        else if (result == WAIT_FAILED) throw std::runtime_error(tr(Msg::SessionInputWaitFailed));
    }
}
void draw_menu(const Console& console, const LaunchOptions& options, std::size_t selected) {
    console.clear();
    console.write(trw(Msg::MenuHeader, {to_utf8(options.configPath.wstring())}));
    try { console.write(config_summary(load_config(options.configPath)) + L"\n"); }
    catch (const std::exception& error) { console.write(trw(Msg::ConfigUnavailable, {error.what()})); }
    try { console.write(trw(Msg::MenuDriverStatus, {to_utf8(driver_label(query_driver_state()))})); }
    catch (const std::exception& error) { console.write(trw(Msg::DriverStatusUnavailable, {error.what()})); }
    constexpr std::array<Msg, 6> labels{Msg::MenuStart, Msg::MenuCheck, Msg::MenuEdit, Msg::MenuInstall, Msg::MenuUninstall, Msg::MenuExit};
    console.write(L"\n");
    for (std::size_t i = 0; i < labels.size(); ++i)
        console.write(std::wstring(i == selected ? L" > " : L"   ") + std::to_wstring(i + 1) + L". " + trw(labels[i]) + L"\n");
    console.write(trw(Msg::MenuNavigation));
}
}

int launch_tui(const LaunchOptions& options) {
    Console console;
    ControlSignals signals;
    try {
        const auto exe = executable_path();
        if (!is_administrator()) {
            console.write(trw(Msg::ElevationPrompt));
            if (elevate_tui(exe, options.configPath, options.explicitConfig) == ElevationResult::Started) return 0;
            console.write(trw(Msg::ElevationCancelled));
            console.wait_key(signals);
            return 1;
        }
        ControllerLock instance;
        if (!options.explicitConfig) {
            try {
                if (ensure_default_config(options.configPath, exe.parent_path() / L"config.example.json")) {
                    console.write(trw(Msg::DefaultConfigCreated));
                    if (!console.wait_key(signals)) return 0;
                }
            } catch (const std::exception& error) {
                console.write(trw(Msg::DefaultConfigUnavailable, {error.what()}));
                if (!console.wait_key(signals)) return 0;
            }
        }
        MenuServices services;
        services.readConfig = [&] { return load_config(options.configPath); };
        services.driverState = query_driver_state;
        services.installDriver = install_driver;
        services.startDriver = start_driver;
        services.uninstallDriver = uninstall_driver;
        services.editConfig = [&] { edit_config(options.configPath); };
        services.tell = [&](const std::wstring& message) { console.write(L"\n" + message + L"\n"); };
        services.confirm = [&](const std::wstring& message) { return confirm(console, signals, message); };
        services.cancelled = [&] { return console.poll_cancel(signals); };
        services.runSession = [&](const Config& config) {
            console.write(trw(Msg::ProxyStartingUi));
            run_proxy_session(config, signals, [&](HANDLE completed, HANDLE ready) { pump_session(console, signals, completed, ready); });
        };
        std::size_t selected = 0;
        bool redraw = true;
        while (!signals.exit_requested()) {
            if (redraw) { draw_menu(console, options, selected); redraw = false; }
            const HANDLE handles[]{signals.stop_event(), console.input()};
            const auto wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1) throw std::runtime_error(tr(Msg::ConsoleMenuWaitFailed));
            const auto record = console.read();
            if (!record) continue;
            if (record->EventType == WINDOW_BUFFER_SIZE_EVENT) { redraw = true; continue; }
            if (record->EventType != KEY_EVENT || !record->Event.KeyEvent.bKeyDown) continue;
            const auto& key = record->Event.KeyEvent;
            const auto action = menu_key(selected, key.wVirtualKeyCode, key.uChar.UnicodeChar);
            if (!action) { if (key.wVirtualKeyCode == VK_UP || key.wVirtualKeyCode == VK_DOWN) redraw = true; continue; }
            if (*action == MenuAction::Exit || !signals.begin_operation()) break;
            set_log_level("info");
            console.write(L"\n");
            bool failed = false;
            try { perform_menu_action(*action, services); }
            catch (const std::exception& error) { console.write(trw(Msg::MenuOperationFailed, {error.what()})); failed = true; }
            const bool cancelled = signals.stop_requested();
            signals.end_operation();
            if (signals.exit_requested()) break;
            if (cancelled) console.write(trw(Msg::MenuOperationStopped));
            if (failed || !cancelled) {
                console.write(trw(Msg::ReturnToMenu));
                if (!console.wait_key(signals)) break;
            }
            redraw = true;
        }
        return 0;
    } catch (const ControllerError& error) {
        console.write(error.already_running()
            ? trw(Msg::UiAlreadyRunning)
            : trw(Msg::UiControllerLockFailed));
        console.wait_key(signals);
        return 1;
    } catch (const std::exception& error) {
        console.write(trw(Msg::MenuOpenFailed, {error.what()}));
        console.wait_key(signals);
        return 1;
    }
}
}
