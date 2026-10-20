#include "l10n.hpp"
#include <windows.h>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ps {
namespace {
struct Message { const char* en; const char* zh; };
constexpr Message messages[] = {
#define PS_MESSAGE(id, en, zh) {en, zh},
#include "messages/core.inc"
#include "messages/config.inc"
#include "messages/ui.inc"
#include "messages/runtime.inc"
#undef PS_MESSAGE
};
static_assert(sizeof(messages) / sizeof(messages[0]) == static_cast<size_t>(Msg::Count));
std::atomic<Language> selected{Language::EnUS};
}

Language language_from_locale(std::wstring_view locale) {
    if (locale.size() >= 2 && (locale[0] == L'z' || locale[0] == L'Z') &&
        (locale[1] == L'h' || locale[1] == L'H') &&
        (locale.size() == 2 || locale[2] == L'-' || locale[2] == L'_')) return Language::ZhCN;
    return Language::EnUS;
}
Language system_language() {
    ULONG count = 0, length = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &length) && length) {
        std::vector<wchar_t> buffer(length);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &length) && count)
            return language_from_locale(buffer.data());
    }
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Language::ZhCN : Language::EnUS;
}
Language current_language() { return selected.load(); }
void set_language(Language language) { selected.store(language == Language::Auto ? system_language() : language); }
const char* language_tag(Language language) {
    return language == Language::ZhCN ? "zh-CN" : language == Language::EnUS ? "en-US" : "auto";
}
std::string_view message_template(Msg id, Language language) {
    const auto index = static_cast<size_t>(id);
    if (index >= static_cast<size_t>(Msg::Count)) throw std::logic_error("Invalid message ID");
    return language == Language::ZhCN ? messages[index].zh : messages[index].en;
}
std::string tr(Msg id, std::initializer_list<std::string> args) {
    const auto source = message_template(id, current_language());
    std::string result;
    for (size_t i = 0; i < source.size();) {
        if (source[i] == '{' && i + 2 < source.size() && source[i + 1] >= '0' && source[i + 1] <= '9') {
            const auto end = source.find('}', i + 1);
            if (end == std::string_view::npos) throw std::logic_error("Invalid message placeholder");
            size_t index = 0;
            for (size_t j = i + 1; j < end; ++j) {
                if (source[j] < '0' || source[j] > '9') throw std::logic_error("Invalid message placeholder");
                index = index * 10 + static_cast<size_t>(source[j] - '0');
                if (index >= args.size()) throw std::logic_error("Missing message argument");
            }
            result += *(args.begin() + index);
            i = end + 1;
        } else result += source[i++];
    }
    return result;
}
std::wstring unicode_text(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::length_error("Text too long");
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length) return L"<invalid UTF-8>";
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}
std::string utf8_text(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::length_error("Text too long");
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return "<invalid Unicode>";
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::wstring trw(Msg id, std::initializer_list<std::string> args) { return unicode_text(tr(id, args)); }
}
