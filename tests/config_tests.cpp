#include "config.hpp"
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
int checks = 0;
void check(bool result, const char* label) { ++checks; if (!result) throw std::runtime_error(label); }
void fails(const std::string& text) {
    ++checks;
    try { (void)ps::parse_config(text); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid configuration was accepted");
}
struct ProbeFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"ProcSocks 语言配置 " + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L".json");
    ~ProbeFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    void write(const std::string& text) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) throw std::runtime_error("Cannot write language probe fixture");
    }
};
std::string error_from(const std::string& text) {
    try { (void)ps::parse_config(text); }
    catch (const std::exception& error) { return error.what(); }
    throw std::runtime_error("Expected configuration rejection");
}
}
int main() {
    try {
        ps::set_language(ps::Language::EnUS);
        auto config = ps::parse_config(R"({"proxy":{"host":"127.0.0.1","port":1080},"processes":["app.exe","C:\\应用\\客户端.exe"]})");
        check(config.includeChildren && config.bypassPrivateNetworks && config.logLevel == "info", "defaults");
        check(config.language == ps::Language::Auto, "language defaults to auto");
        check(config.processes.size() == 2 && config.processes[1] == L"C:\\应用\\客户端.exe", "unicode paths");
        check(!config.proxy.authenticate && config.proxy.port == 1080, "proxy defaults");
        config = ps::parse_config(R"({"proxy":{"host":"::1","port":1,"username":"user","password":"pass"},"processes":["app.exe"],"includeChildren":false,"bypassPrivateNetworks":false,"logLevel":"debug"})");
        check(!config.includeChildren && !config.bypassPrivateNetworks && config.proxy.authenticate, "explicit flags/auth");
        fails(R"({})");
        fails(R"({"proxy":{"host":"localhost","port":0},"processes":["a.exe"]})");
        fails(R"({"proxy":{"host":"localhost","port":65536},"processes":["a.exe"]})");
        fails(R"({"proxy":{"host":"localhost","port":1.2},"processes":["a.exe"]})");
        fails(R"({"proxy":{"host":"localhost","port":1080},"processes":[]})");
        fails(R"({"proxy":{"host":"socks5://localhost","port":1080},"processes":["a.exe"]})");
        for (const auto* host : {"localhost:1080", "[::1]", "bad\\\\host", "bad..host", "bad\\u0001host", "bad\\u007fhost", "bad\\u00a0host", "fe80::1%abc", "fe80::1%4294967296"})
            fails(std::string(R"({"proxy":{"host":")") + host + R"(","port":1080},"processes":["a.exe"]})");
        for (const auto* host : {"proxy.example.com", "localhost.", "::ffff:127.0.0.1", "fe80::1%12"}) {
            const auto valid = ps::parse_config(std::string(R"({"proxy":{"host":")") + host + R"(","port":1080},"processes":["a.exe"]})");
            check(valid.proxy.host == host, "valid host");
        }
        fails(R"({"proxy":{"host":"localhost","port":1080,"username":"x"},"processes":["a.exe"]})");
        fails(R"({"proxy":{"host":"localhost","port":1080,"username":"","password":"x"},"processes":["a.exe"]})");
        fails(R"({"proxy":{"host":"localhost","port":1080},"processes":["*app.exe"]})");
        fails(R"({"proxy":{"host":"localhost","port":1080},"processes":["relative/app.exe"]})");
        fails(R"({"proxy":{"host":"localhost","port":1080},"processes":["a.exe"],"includeChildren":"false"})");
        fails(R"({"proxy":{"host":"localhost","port":1080},"processes":["a.exe"],"logLevel":"invalid"})");
        fails(R"({"proxy":{"host":"localhost","port":1080},"processes":["a.exe"],"typo":true})");
        fails(R"({"proxy":{"host":"localhost","port":1080,"port":1081},"processes":["a.exe"]})");
        try {
            (void)ps::parse_config(R"({"proxy":{"password":"TOPSECRET" oops}})");
            check(false, "parse error missing");
        } catch (const std::exception& error) { check(std::string(error.what()).find("TOPSECRET") == std::string::npos, "secret in parse diagnostic"); }

        for (const auto language : {ps::Language::Auto, ps::Language::ZhCN, ps::Language::EnUS}) {
            const std::string text = std::string(R"({"proxy":{"host":"localhost","port":1080},"processes":["a.exe"],"language":")") + ps::language_tag(language) + "\"}";
            check(ps::parse_config(text).language == language, "explicit language accepted");
            check(ps::current_language() == ps::Language::EnUS, "config parse does not change startup language");
        }
        for (const auto* value : {"null", "true", "1", "[]", "{}", "\"zh\"", "\"en\"", "\"ZH-CN\"", "\"\""})
            fails(std::string(R"({"proxy":{"host":"localhost","port":1080},"processes":["a.exe"],"language":)") + value + "}");

        ProbeFile probe;
        const auto system = ps::system_language();
        const auto other = system == ps::Language::EnUS ? ps::Language::ZhCN : ps::Language::EnUS;
        const std::string otherPrefix = std::string("{\"language\":\"") + ps::language_tag(other) + "\",";
        check(ps::config_language(probe.path) == system, "unreadable config uses system language");
        probe.write("{}");
        check(ps::config_language(probe.path) == system, "omitted language uses system language");
        probe.write(R"({"language":"auto"})");
        check(ps::config_language(probe.path) == system, "auto uses system language");
        probe.write(R"({"language":"zh-CN","proxy":"bad"})");
        check(ps::config_language(probe.path) == ps::Language::ZhCN, "Chinese selected despite invalid other fields");
        probe.write(R"({"language":"en-US","processes":[]})");
        check(ps::config_language(probe.path) == ps::Language::EnUS, "English selected despite invalid other fields");
        for (const auto& invalid : {
            otherPrefix + R"("proxy":{"password":"TOPSECRET" oops}})",
            otherPrefix + R"("language":"auto"})",
            otherPrefix + R"("proxy":{"port":1,"port":2}})",
            otherPrefix + R"("unknown":{"TOPSECRET":1,"TOPSECRET":2}})",
            std::string(R"({"language":"TOPSECRET"})"),
            std::string(R"({"language":null})"),
            std::string("[]"),
            otherPrefix + "\"padding\":\"" + std::string(1024 * 1024, 'x') + "\"}"}) {
            probe.write(invalid);
            check(ps::config_language(probe.path) == system, "invalid or ambiguous JSON uses system language");
        }
        for (const auto language : {ps::Language::ZhCN, ps::Language::EnUS}) {
            const auto text = std::string("{\"language\":\"") + ps::language_tag(language) + "\"}";
            probe.write(text);
            ps::set_language(ps::config_language(probe.path));
            check(error_from(text) == ps::tr(ps::Msg::ConfigRequired), "valid language localizes other field error");
            check(error_from(text).find(language == ps::Language::ZhCN ? "必须" : "required") != std::string::npos,
                "configuration diagnostic has selected language");
            for (const auto& invalid : {
                std::string(R"({"proxy":{"password":"TOPSECRET" oops}})"),
                std::string(R"({"proxy":{"host":"localhost","port":1080,"username":"TOPSECRET"},"processes":["a.exe"]})"),
                std::string(R"({"proxy":{"host":"localhost","port":1080,"username":"ACCOUNTSECRET","password":"PASSWORDSECRET"},"processes":[]})"),
                std::string(R"({"language":"TOPSECRET"})"),
                std::string(R"({"TOPSECRET":1})"),
                std::string(R"({"proxy":{"TOPSECRET":1,"TOPSECRET":2}})"),
                std::string(R"({"language":"en-US","proxy":{"password":"TOPSECRET"},"number":1e100000})")}) {
                const auto diagnostic = error_from(invalid);
                check(diagnostic.find("TOPSECRET") == std::string::npos && diagnostic.find("ACCOUNTSECRET") == std::string::npos &&
                    diagnostic.find("PASSWORDSECRET") == std::string::npos, "diagnostic does not reveal credentials or untrusted keys");
            }
        }
        ps::set_language(ps::Language::EnUS);
        probe.write(R"({"language":"zh-CN","proxy":{"host":"localhost","port":1080},"processes":["a.exe"]})");
        check(ps::load_config(probe.path).language == ps::Language::ZhCN, "load accepts Unicode/space configuration path");
        check(ps::current_language() == ps::Language::EnUS, "reload keeps startup display language");
        std::cout << "config_tests: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "config_tests FAILED: " << error.what() << '\n'; return 1; }
}
