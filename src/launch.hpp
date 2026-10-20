#pragma once
#include "l10n.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace ps {
enum class Command { Help, Version, Check, Run, Install, Uninstall, Tui, Invalid };
struct LaunchOptions {
    Command command = Command::Invalid;
    std::filesystem::path configPath;
    bool explicitConfig = false;
    // Resolved language forwarded only by the TUI's elevation launcher.
    Language uiLanguage = Language::Auto;
    int helpExitCode = 0;
};
LaunchOptions parse_launch(const std::vector<std::wstring>& args, bool interactive,
                           const std::filesystem::path& exePath, const std::filesystem::path& callerCwd);
std::filesystem::path executable_path();
bool interactive_console();
std::wstring quote_windows_argument(const std::wstring& argument);
std::wstring tui_elevation_arguments(const std::filesystem::path& absoluteConfig, bool explicitConfig,
                                     Language language = current_language());
enum class ElevationResult { Started, Cancelled };
ElevationResult elevate_tui(const std::filesystem::path& exe, const std::filesystem::path& absoluteConfig,
                            bool explicitConfig = true, Language language = current_language());
bool ensure_default_config(const std::filesystem::path& config, const std::filesystem::path& example);
void edit_config(const std::filesystem::path& config);
}
