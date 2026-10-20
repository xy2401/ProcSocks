#pragma once
#include "l10n.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps {
struct ProxyConfig {
    std::string host;
    std::uint16_t port = 1080;
    bool authenticate = false;
    std::string username;
    std::string password;
};
struct Config {
    Language language = Language::Auto;
    ProxyConfig proxy;
    std::vector<std::wstring> processes;
    bool includeChildren = true;
    bool bypassPrivateNetworks = true;
    std::string logLevel = "info";
};
Config parse_config(const std::string& text);
Config load_config(const std::filesystem::path& path);
// Resolve only the startup display language. All JSON syntax and duplicate keys
// are checked before trusting it; other configuration fields may still be invalid.
// This probe never changes the process-wide display language.
Language config_language(const std::filesystem::path& path) noexcept;
std::wstring from_utf8(const std::string& text);
std::string to_utf8(const std::wstring& text);
}
