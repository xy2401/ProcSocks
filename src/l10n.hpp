#pragma once
#include <initializer_list>
#include <string>
#include <string_view>

namespace ps {
enum class Language { Auto, ZhCN, EnUS };
enum class Msg {
#define PS_MESSAGE(id, en, zh) id,
#include "messages/core.inc"
#include "messages/config.inc"
#include "messages/ui.inc"
#include "messages/runtime.inc"
#undef PS_MESSAGE
    Count
};
Language system_language();
Language language_from_locale(std::wstring_view locale);
Language current_language();
void set_language(Language language);
const char* language_tag(Language language);
std::string tr(Msg id, std::initializer_list<std::string> args = {});
std::wstring trw(Msg id, std::initializer_list<std::string> args = {});
// Read-only catalog access for completeness/placeholder regression tests.
std::string_view message_template(Msg id, Language language);
std::wstring unicode_text(const std::string& utf8);
std::string utf8_text(const std::wstring& unicode);
}
