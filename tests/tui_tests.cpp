#include "net.hpp"
#include "tui.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
void check(bool good, const char* label) { ++checks; if (!good) throw std::runtime_error(label); }
template<typename F> void fails(F&& action, const char* expected) {
    bool caught = false;
    try { action(); }
    catch (const std::exception& error) { caught = std::string(error.what()) == expected; }
    check(caught, "expected workflow error was not propagated");
}
ps::Config make_config() {
    ps::Config config;
    config.proxy.host = "127.0.0.1"; config.proxy.port = 1080;
    config.processes = {L"launcher.exe", L"C:\\应用\\client.exe"};
    return config;
}
struct Fixture {
    ps::Config config = make_config();
    ps::DriverState state = ps::DriverState::Running;
    bool answer = true, cancelled = false;
    bool invalid = false, failInstall = false, failStart = false, failSession = false;
    std::vector<std::string> calls;
    std::vector<std::wstring> questions, messages;
    std::vector<ps::Config> sessionConfigs;
    std::function<void()> afterRead, afterConfirm, afterInstall, afterStart, afterEdit;
    ps::MenuServices services;

    Fixture() {
        services.cancelled = [&] { return cancelled; };
        services.readConfig = [&] {
            calls.emplace_back("read");
            if (invalid) throw std::runtime_error("invalid test configuration");
            if (afterRead) afterRead();
            return config;
        };
        services.driverState = [&] { calls.emplace_back("state"); return state; };
        services.confirm = [&](const std::wstring& message) {
            calls.emplace_back("confirm"); questions.push_back(message);
            if (afterConfirm) afterConfirm();
            return answer;
        };
        services.installDriver = [&] {
            calls.emplace_back("install");
            if (failInstall) throw std::runtime_error("driver installation failed");
            state = ps::DriverState::Running;
            if (afterInstall) afterInstall();
        };
        services.startDriver = [&] {
            calls.emplace_back("start-driver");
            if (failStart) throw std::runtime_error("driver start failed");
            state = ps::DriverState::Running;
            if (afterStart) afterStart();
        };
        services.uninstallDriver = [&] { calls.emplace_back("uninstall"); state = ps::DriverState::NotInstalled; };
        services.editConfig = [&] { calls.emplace_back("edit"); if (afterEdit) afterEdit(); };
        services.tell = [&](const std::wstring& message) { calls.emplace_back("tell"); messages.push_back(message); };
        services.runSession = [&](const ps::Config& value) {
            calls.emplace_back("session"); sessionConfigs.push_back(value);
            if (failSession) throw std::runtime_error("session startup failed");
        };
    }
    void action(ps::MenuAction action) { ps::perform_menu_action(action, services); }
    void expect(std::initializer_list<const char*> expected) {
        std::vector<std::string> values;
        for (const auto* value : expected) values.emplace_back(value);
        check(calls == values, "menu workflow calls/order differ");
    }
};

void navigation_test() {
    using ps::MenuAction;
    std::size_t selected = 0;
    check(!ps::menu_key(selected, VK_UP, 0) && selected == 5, "up did not wrap to exit");
    check(ps::menu_key(selected, VK_RETURN, 0) == MenuAction::Exit, "Enter did not activate selected item");
    check(!ps::menu_key(selected, VK_DOWN, 0) && selected == 0, "down did not wrap to start");
    for (std::size_t i = 0; i != 6; ++i) {
        selected = i;
        check(ps::menu_key(selected, VK_RETURN, L'\r') == static_cast<MenuAction>(i), "Enter enum mapping");
        selected = 5 - i;
        check(ps::menu_key(selected, static_cast<unsigned short>('1' + i), static_cast<wchar_t>(L'1' + i)) == static_cast<MenuAction>(i)
              && selected == i, "numeric key did not activate its action");
    }
    selected = 2;
    check(ps::menu_key(selected, VK_ESCAPE, 0) == MenuAction::Exit && selected == 2, "Esc did not exit");
    check(!ps::menu_key(selected, 'X', L'x') && selected == 2, "unrecognized key changed selection");
    check(!ps::menu_key(selected, '0', L'0') && selected == 2, "zero accepted as menu action");
    check(!ps::menu_key(selected, '7', L'7') && selected == 2, "out-of-range numeric action accepted");
    selected = 6001;
    check(ps::menu_key(selected, VK_RETURN, 0) == MenuAction::Check && selected == 1, "selection normalization failed");
}
void summary_test() {
    auto config = make_config();
    config.proxy.host = "2001:db8::1";
    config.proxy.authenticate = true;
    config.proxy.username = "DO_NOT_SHOW_USERNAME";
    config.proxy.password = "DO_NOT_SHOW_PASSWORD";
    const auto summary = ps::config_summary(config);
    check(summary.find(L"[2001:db8::1]:1080") != std::wstring::npos, "IPv6 endpoint summary is ambiguous");
    check(summary.find(L"已配置") != std::wstring::npos, "authentication presence missing");
    check(summary.find(L"DO_NOT_SHOW_USERNAME") == std::wstring::npos && summary.find(L"DO_NOT_SHOW_PASSWORD") == std::wstring::npos,
          "summary exposed proxy credentials");
    check(summary.find(L"进程规则：2") != std::wstring::npos, "process rule count missing");
    check(summary.find(L"子进程继承：开启") != std::wstring::npos && summary.find(L"局域网直连：开启") != std::wstring::npos,
          "enabled configuration flags missing");
    config.proxy.host = "proxy.example.com"; config.proxy.authenticate = false;
    config.includeChildren = false; config.bypassPrivateNetworks = false;
    const auto disabled = ps::config_summary(config);
    check(disabled.find(L"proxy.example.com:1080") != std::wstring::npos, "hostname summary missing");
    check(disabled.find(L"认证：无") != std::wstring::npos && disabled.find(L"子进程继承：关闭") != std::wstring::npos &&
          disabled.find(L"局域网直连：关闭") != std::wstring::npos, "disabled configuration flags missing");
}
void bilingual_summary_test() {
    auto config = make_config();
    config.language = ps::Language::ZhCN;
    config.proxy.authenticate = true;
    config.proxy.username = "DO_NOT_SHOW_USERNAME";
    config.proxy.password = "DO_NOT_SHOW_PASSWORD";
    ps::set_language(ps::Language::EnUS);
    const auto english = ps::config_summary(config);
    check(english.find(L"Process rules: 2") != std::wstring::npos && english.find(L"Authentication: configured") != std::wstring::npos,
          "English summary did not use selected language");
    check(english.find(L"DO_NOT_SHOW_USERNAME") == std::wstring::npos && english.find(L"DO_NOT_SHOW_PASSWORD") == std::wstring::npos,
          "English summary exposed credentials");
    check(ps::current_language() == ps::Language::EnUS, "config summary changed the startup language");
    Fixture fixture;
    fixture.config.language = ps::Language::ZhCN;
    fixture.action(ps::MenuAction::Check);
    check(fixture.messages.back().find(L"Configuration check passed.") != std::wstring::npos,
          "menu check language changed with reloaded config");
    ps::set_language(ps::Language::ZhCN);
}
void start_workflow_test() {
    for (const auto state : {ps::DriverState::Running, ps::DriverState::Stopped, ps::DriverState::Pending}) {
        Fixture f; f.state = state;
        f.action(ps::MenuAction::Start);
        f.expect({"read", "state", "start-driver", "session"});
        check(f.questions.empty(), "existing driver asked to install");
        check(f.sessionConfigs.size() == 1 && f.sessionConfigs[0].processes == f.config.processes, "session received wrong configuration");
    }
    {
        Fixture f; f.state = ps::DriverState::NotInstalled;
        f.action(ps::MenuAction::Start);
        f.expect({"read", "state", "confirm", "install", "start-driver", "session"});
        check(f.questions.size() == 1, "missing-driver installation not confirmed");
    }
    {
        Fixture f; f.state = ps::DriverState::NotInstalled; f.answer = false;
        f.action(ps::MenuAction::Start);
        f.expect({"read", "state", "confirm"});
        check(f.sessionConfigs.empty() && f.state == ps::DriverState::NotInstalled, "declining installation started or installed driver");
    }
}
void menu_actions_test() {
    {
        Fixture f; f.action(ps::MenuAction::Check);
        f.expect({"read", "tell"});
        check(f.messages.size() == 1 && f.messages[0].find(L"检查通过") != std::wstring::npos, "configuration check result missing");
    }
    {
        Fixture f; f.action(ps::MenuAction::Edit); f.expect({"edit", "tell"});
    }
    {
        Fixture f; f.action(ps::MenuAction::Install); f.expect({"install", "tell"});
    }
    {
        Fixture f; f.action(ps::MenuAction::Uninstall); f.expect({"confirm", "uninstall", "tell"});
        check(f.questions.size() == 1 && f.state == ps::DriverState::NotInstalled, "confirmed uninstall missing");
    }
    {
        Fixture f; f.answer = false; f.action(ps::MenuAction::Uninstall); f.expect({"confirm"});
        check(f.state == ps::DriverState::Running, "declined uninstall changed state");
    }
    {
        Fixture f; f.action(ps::MenuAction::Exit); f.expect({});
    }
}
void failure_and_reload_test() {
    for (const auto action : {ps::MenuAction::Start, ps::MenuAction::Check}) {
        Fixture f; f.invalid = true;
        fails([&] { f.action(action); }, "invalid test configuration");
        f.expect({"read"});
        check(f.questions.empty() && f.sessionConfigs.empty(), "invalid configuration caused driver/session side effects");
    }
    {
        Fixture f; f.state = ps::DriverState::NotInstalled; f.failInstall = true;
        fails([&] { f.action(ps::MenuAction::Start); }, "driver installation failed");
        f.expect({"read", "state", "confirm", "install"});
    }
    {
        Fixture f; f.failStart = true;
        fails([&] { f.action(ps::MenuAction::Start); }, "driver start failed");
        f.expect({"read", "state", "start-driver"});
        f.failStart = false; f.calls.clear(); f.config.proxy.port = 2080;
        f.action(ps::MenuAction::Start);
        f.expect({"read", "state", "start-driver", "session"});
        check(f.sessionConfigs.back().proxy.port == 2080, "driver-start retry did not reload configuration");
    }
    {
        Fixture f; f.failSession = true;
        fails([&] { f.action(ps::MenuAction::Start); }, "session startup failed");
        f.expect({"read", "state", "start-driver", "session"});
        f.failSession = false; f.calls.clear();
        f.config.proxy.port = 3080; f.config.processes = {L"changed.exe"};
        f.action(ps::MenuAction::Start);
        f.expect({"read", "state", "start-driver", "session"});
        check(f.sessionConfigs.size() == 2 && f.sessionConfigs[0].proxy.port == 1080 && f.sessionConfigs[1].proxy.port == 3080 &&
              f.sessionConfigs[1].processes == std::vector<std::wstring>{L"changed.exe"}, "failed-session retry reused stale configuration");
    }
    {
        Fixture f;
        f.afterEdit = [&] { f.config.includeChildren = false; f.config.proxy.port = 4080; };
        f.action(ps::MenuAction::Edit); f.calls.clear();
        f.action(ps::MenuAction::Check);
        check(f.messages.back().find(L":4080") != std::wstring::npos && f.messages.back().find(L"子进程继承：关闭") != std::wstring::npos,
              "configuration check after editing used stale data");
        f.calls.clear(); f.action(ps::MenuAction::Start);
        check(f.sessionConfigs.back().proxy.port == 4080 && !f.sessionConfigs.back().includeChildren, "start after editing used stale data");
    }
}
void cancellation_test() {
    for (const auto action : {ps::MenuAction::Start, ps::MenuAction::Check, ps::MenuAction::Edit, ps::MenuAction::Install, ps::MenuAction::Uninstall}) {
        Fixture f; f.cancelled = true; f.action(action); f.expect({});
    }
    {
        Fixture f; f.afterRead = [&] { f.cancelled = true; };
        f.action(ps::MenuAction::Start); f.expect({"read"});
    }
    {
        Fixture f; f.state = ps::DriverState::NotInstalled;
        f.afterConfirm = [&] { f.cancelled = true; };
        f.action(ps::MenuAction::Start); f.expect({"read", "state", "confirm"});
    }
    {
        Fixture f; f.state = ps::DriverState::NotInstalled;
        f.afterInstall = [&] { f.cancelled = true; };
        f.action(ps::MenuAction::Start); f.expect({"read", "state", "confirm", "install"});
        check(f.sessionConfigs.empty(), "cancel during installation started session");
    }
    {
        Fixture f; f.afterStart = [&] { f.cancelled = true; };
        f.action(ps::MenuAction::Start); f.expect({"read", "state", "start-driver"});
        check(f.sessionConfigs.empty(), "cancel during driver start started session");
    }
    {
        Fixture f; f.afterConfirm = [&] { f.cancelled = true; };
        f.action(ps::MenuAction::Uninstall); f.expect({"confirm"});
        check(f.state == ps::DriverState::Running, "cancelled uninstall changed driver");
    }
}
}

int main() {
    ps::set_language(ps::Language::ZhCN);
    try {
        navigation_test();
        summary_test();
        bilingual_summary_test();
        start_workflow_test();
        menu_actions_test();
        failure_and_reload_test();
        cancellation_test();
        std::cout << "tui_tests: " << checks << " checks passed (injected services; no driver, ETW, UAC, editor or global mutex touched)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tui_tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
