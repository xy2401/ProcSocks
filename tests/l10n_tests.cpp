#include "l10n.hpp"
#include "log.hpp"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
int checks = 0;
void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
std::map<unsigned, unsigned> placeholders(std::string_view text) {
    std::map<unsigned, unsigned> result;
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != '{' || text[i + 1] < '0' || text[i + 1] > '9') continue;
        const auto end = text.find('}', i);
        require(end != std::string_view::npos, "unclosed placeholder");
        unsigned index = 0;
        for (size_t j = i + 1; j < end; ++j) {
            require(text[j] >= '0' && text[j] <= '9', "invalid placeholder");
            index = index * 10 + text[j] - '0';
        }
        require(index < 10, "unexpected placeholder index");
        ++result[index];
        i = end;
    }
    return result;
}
struct Redirect {
    DWORD channel;
    HANDLE previous;
    HANDLE file = INVALID_HANDLE_VALUE;
    explicit Redirect(DWORD channel_) : channel(channel_), previous(GetStdHandle(channel_)) {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        const auto path = std::filesystem::path(executable).parent_path() /
            (L"utf8-output-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(channel) + L".tmp");
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE || !SetStdHandle(channel, file)) throw std::runtime_error("capture setup failed");
    }
    ~Redirect() { SetStdHandle(channel, previous); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    std::string read() {
        LARGE_INTEGER size{}, start{};
        if (!GetFileSizeEx(file, &size) || !SetFilePointerEx(file, start, nullptr, FILE_BEGIN))
            throw std::runtime_error("capture read setup failed");
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        DWORD count = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
            throw std::runtime_error("capture read failed");
        return bytes;
    }
};
}
int main() {
    try {
        using namespace ps;
        for (const auto* locale : {L"zh-CN", L"zh-TW", L"zh-Hant-HK", L"ZH_cn", L"zh"})
            require(language_from_locale(locale) == Language::ZhCN, "Chinese UI mapping");
        for (const auto* locale : {L"en-US", L"ja-JP", L"fr-FR", L"", L"zho", L"unknown"})
            require(language_from_locale(locale) == Language::EnUS, "UI language English fallback");
        set_language(Language::Auto);
        require(current_language() == system_language(), "auto resolves system UI language");
        for (size_t i = 0; i < static_cast<size_t>(Msg::Count); ++i) {
            const auto id = static_cast<Msg>(i);
            const auto en = message_template(id, Language::EnUS), zh = message_template(id, Language::ZhCN);
            require(!en.empty() && !zh.empty(), "missing translation");
            require(placeholders(en) == placeholders(zh), "translation placeholder mismatch");
            require(utf8_text(unicode_text(std::string(en))) == en, "English invalid UTF8");
            require(utf8_text(unicode_text(std::string(zh))) == zh, "Chinese invalid UTF8");
            for (auto language : {Language::EnUS, Language::ZhCN}) {
                set_language(language);
                const auto formatted = tr(id, {"arg0", "arg1", "arg2", "arg3", "arg4", "arg5", "arg6", "arg7", "arg8", "arg9"});
                require(placeholders(formatted).empty(), "unexpanded placeholder");
                require(trw(id, {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"}).size() > 0, "empty wide message");
            }
        }
        set_language(Language::EnUS);
        require(tr(Msg::OutputFailed, {"{1}"}).find("{1}") != std::string::npos, "arguments recursively expanded");
        set_language(Language::ZhCN);
        {
            Redirect output(STD_OUTPUT_HANDLE), error(STD_ERROR_HANDLE);
            write_stdout_message("中文输出 🧪\n");
            write_console_message(L"配置目录：C:\\中文 空格\\config.json\n");
            write_stderr_message("中文错误\n");
            const auto out = output.read(), err = error.read();
            require(out == "中文输出 🧪\n配置目录：C:\\中文 空格\\config.json\n", "stdout UTF8 bytes or newline mismatch");
            require(err == "中文错误\n", "stderr UTF8 bytes/channel mismatch");
        }
        {
            Redirect error(STD_ERROR_HANDLE);
            set_log_level("info");
            std::vector<std::thread> writers;
            for (int i = 0; i < 4; ++i) writers.emplace_back([i] {
                for (int j = 0; j < 20; ++j) log_info("中文记录 " + std::to_string(i) + ":" + std::to_string(j));
            });
            for (auto& writer : writers) writer.join();
            log_error("错误\n不得换行\t结束");
            const auto bytes = error.read();
            require(std::count(bytes.begin(), bytes.end(), '\n') == 81, "concurrent log record framing");
            require(bytes.find("[ERROR] 错误 不得换行 结束\n") != std::string::npos, "error tag/sanitization");
            for (int i = 0; i < 4; ++i) for (int j = 0; j < 20; ++j)
                require(bytes.find("[INFO] 中文记录 " + std::to_string(i) + ":" + std::to_string(j) + "\n") != std::string::npos,
                        "concurrent Unicode log corrupted");
            require(utf8_text(unicode_text(bytes)) == bytes, "log UTF8 invalid");
        }
        std::cout << "Localization tests passed: " << checks << " checks, " << static_cast<size_t>(Msg::Count) << " messages\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
