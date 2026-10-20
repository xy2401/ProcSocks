#pragma once
#include "config.hpp"
#include "driver.hpp"
#include "launch.hpp"
#include <functional>
#include <optional>
namespace ps {
enum class MenuAction { Start, Check, Edit, Install, Uninstall, Exit };
// UI-independent actions allow driver-free tests of the menu workflow.
struct MenuServices {
    std::function<Config()> readConfig;
    std::function<DriverState()> driverState;
    std::function<void()> installDriver, startDriver, uninstallDriver, editConfig;
    std::function<void(const Config&)> runSession;
    std::function<bool(const std::wstring&)> confirm;
    std::function<void(const std::wstring&)> tell;
    std::function<bool()> cancelled;
};
std::optional<MenuAction> menu_key(std::size_t& selected, unsigned short key, wchar_t character);
std::wstring config_summary(const Config& config);
void perform_menu_action(MenuAction action, const MenuServices& services);
int launch_tui(const LaunchOptions& options);
}
