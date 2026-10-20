#include "config.hpp"
#include "net.hpp"
#include "json.hpp"
#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace ps {
namespace {
using Json = nlohmann::json;
void fields(const Json& value, std::initializer_list<const char*> allowed, const char* name) {
    if (!value.is_object()) throw std::runtime_error(tr(Msg::ConfigObject, {name}));
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool found = false;
        for (const auto* field : allowed) if (it.key() == field) found = true;
        if (!found) throw std::runtime_error(tr(Msg::ConfigUnknownField, {name}));
    }
}
std::string string_value(const Json& value, const std::string& name) {
    if (!value.is_string()) throw std::runtime_error(tr(Msg::ConfigString, {name}));
    auto result = value.get<std::string>();
    if (result.find('\0') != std::string::npos) throw std::runtime_error(tr(Msg::ConfigNul, {name}));
    (void)from_utf8(result);
    return result;
}
bool boolean(const Json& object, const char* name, bool fallback) {
    if (!object.contains(name)) return fallback;
    if (!object.at(name).is_boolean()) throw std::runtime_error(tr(Msg::ConfigBoolean, {name}));
    return object.at(name).get<bool>();
}
bool valid_host(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    auto wide = from_utf8(host);
    std::vector<WORD> types(wide.size());
    if (!GetStringTypeW(CT_CTYPE1, wide.data(), static_cast<int>(wide.size()), types.data())) return false;
    for (auto type : types) if (type & (C1_SPACE | C1_CNTRL)) return false;
    if (host.find(':') != std::string::npos) {
        auto numeric = host;
        const auto scope = numeric.find('%');
        if (scope != std::string::npos) {
            const auto zone = numeric.substr(scope + 1);
            if (zone.empty() || zone.size() > 10 || zone.find_first_not_of("0123456789") != std::string::npos) return false;
            if (std::stoull(zone) > 0xffffffffULL) return false;
            numeric.resize(scope);
        }
        IN6_ADDR address{};
        return InetPtonA(AF_INET6, numeric.c_str(), &address) == 1;
    }
    // Keep check entirely offline. Unicode DNS labels are resolved by Winsock at run time.
    for (unsigned char c : host)
        if (c < 128 && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
            !(c >= '0' && c <= '9') && c != '-' && c != '_' && c != '.') return false;
    std::size_t start = 0;
    while (start < host.size()) {
        auto end = host.find('.', start);
        if (end == std::string::npos) end = host.size();
        if (end == start || end - start > 63 || host[start] == '-' || host[end - 1] == '-') return false;
        start = end + 1;
    }
    return true;
}
Json parse_json(const std::string& text) {
    if (text.size() > 1024 * 1024) throw std::runtime_error(tr(Msg::ConfigSizeLimit));
    try {
        // Reject duplicates throughout the document before trusting its language.
        std::vector<std::set<std::string>> keys;
        auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            else if (event == Json::parse_event_t::object_end) keys.pop_back();
            else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error(tr(Msg::ConfigDuplicateKey));
            return true;
        };
        return Json::parse(text, callback);
    } catch (const Json::parse_error& error) {
        // Library diagnostics may include credentials from malformed JSON.
        throw std::runtime_error(tr(Msg::ConfigInvalidJson, {std::to_string(error.byte)}));
    } catch (const Json::exception&) {
        // For example, numeric overflow in otherwise valid JSON. Never echo input.
        throw std::runtime_error(tr(Msg::ConfigInvalidJson, {"0"}));
    }
}
Language parse_language(const Json& root) {
    if (!root.contains("language")) return Language::Auto;
    const auto& value = root.at("language");
    if (value.is_string()) {
        const auto& language = value.get_ref<const std::string&>();
        if (language == "auto") return Language::Auto;
        if (language == "zh-CN") return Language::ZhCN;
        if (language == "en-US") return Language::EnUS;
    }
    throw std::runtime_error(tr(Msg::ConfigLanguage));
}
std::string read_config(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error(tr(Msg::ConfigOpen));
    std::string text;
    char buffer[4096];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount()) {
        text.append(buffer, static_cast<std::size_t>(stream.gcount()));
        if (text.size() > 1024 * 1024) throw std::runtime_error(tr(Msg::ConfigSizeLimit));
    }
    if (stream.bad()) throw std::runtime_error(tr(Msg::ConfigRead));
    return text;
}
}
std::wstring from_utf8(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > 1024 * 1024) throw std::runtime_error(tr(Msg::ConfigStringLimit));
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error(tr(Msg::ConfigUtf8));
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return tr(Msg::ConfigUnicode);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
Config parse_config(const std::string& text) {
    const Json root = parse_json(text);
    fields(root, {"proxy", "processes", "includeChildren", "bypassPrivateNetworks", "logLevel", "language"}, "config");
    Config result;
    result.language = parse_language(root);
    if (!root.contains("proxy") || !root.contains("processes")) throw std::runtime_error(tr(Msg::ConfigRequired));
    const auto& proxy = root.at("proxy");
    fields(proxy, {"host", "port", "username", "password"}, "proxy");
    if (!proxy.contains("host") || !proxy.contains("port")) throw std::runtime_error(tr(Msg::ConfigProxyRequired));
    result.proxy.host = string_value(proxy.at("host"), "proxy.host");
    if (!valid_host(result.proxy.host))
        throw std::runtime_error(tr(Msg::ConfigHost));
    if (!proxy.at("port").is_number_integer()) throw std::runtime_error(tr(Msg::ConfigPort));
    const auto port = proxy.at("port").get<std::int64_t>();
    if (port < 1 || port > 65535) throw std::runtime_error(tr(Msg::ConfigPort));
    result.proxy.port = static_cast<std::uint16_t>(port);
    if (proxy.contains("username") != proxy.contains("password")) throw std::runtime_error(tr(Msg::ConfigAuthPair));
    if (proxy.contains("username")) {
        result.proxy.authenticate = true;
        result.proxy.username = string_value(proxy.at("username"), "proxy.username");
        result.proxy.password = string_value(proxy.at("password"), "proxy.password");
        if (result.proxy.username.empty() || result.proxy.username.size() > 255 || result.proxy.password.empty() || result.proxy.password.size() > 255)
            throw std::runtime_error(tr(Msg::ConfigAuthSize));
    }
    const auto& processes = root.at("processes");
    if (!processes.is_array() || processes.empty() || processes.size() > 256) throw std::runtime_error(tr(Msg::ConfigProcesses));
    for (const auto& value : processes) {
        auto process = from_utf8(string_value(value, tr(Msg::ConfigProcessEntry)));
        std::replace(process.begin(), process.end(), L'/', L'\\');
        if (process.empty() || process.size() > 32767 || process.find_first_of(L"*\r\n\t\"") != std::wstring::npos)
            throw std::runtime_error(tr(Msg::ConfigProcessInvalid));
        // Permit the extended-length prefix, but no wildcard question marks.
        const auto plain = process.rfind(L"\\\\?\\", 0) == 0 ? process.substr(4) : process;
        if (plain.find(L'?') != std::wstring::npos || process.back() == L'\\') throw std::runtime_error(tr(Msg::ConfigProcessInvalid));
        const bool hasSeparator = process.find(L'\\') != std::wstring::npos;
        const bool drive = process.size() >= 3 && ((process[0] >= L'A' && process[0] <= L'Z') || (process[0] >= L'a' && process[0] <= L'z')) && process[1] == L':' && process[2] == L'\\';
        const bool unc = process.size() > 3 && process[0] == L'\\' && process[1] == L'\\';
        if ((hasSeparator && !drive && !unc) || (!hasSeparator && process.find(L':') != std::wstring::npos))
            throw std::runtime_error(tr(Msg::ConfigProcessAbsolute));
        if (hasSeparator) process = std::filesystem::path(process).lexically_normal().wstring();
        result.processes.push_back(std::move(process));
    }
    result.includeChildren = boolean(root, "includeChildren", true);
    result.bypassPrivateNetworks = boolean(root, "bypassPrivateNetworks", true);
    if (root.contains("logLevel")) result.logLevel = string_value(root.at("logLevel"), "logLevel");
    if (result.logLevel != "error" && result.logLevel != "info" && result.logLevel != "debug") throw std::runtime_error(tr(Msg::ConfigLogLevel));
    return result;
}
Config load_config(const std::filesystem::path& path) {
    return parse_config(read_config(path));
}
Language config_language(const std::filesystem::path& path) noexcept {
    const auto fallback = system_language();
    try {
        const auto root = parse_json(read_config(path));
        if (!root.is_object()) return fallback;
        const auto language = parse_language(root);
        return language == Language::Auto ? fallback : language;
    } catch (...) {
        return fallback;
    }
}
}
