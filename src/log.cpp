#include "log.hpp"
#include "l10n.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
namespace ps {
namespace {
std::mutex outputMutex;
std::atomic_int level{1};
// Caller holds outputMutex so complete records and menu messages cannot interleave.
void write_output(DWORD channel, const std::wstring& message) {
    const auto output = GetStdHandle(channel);
    DWORD mode = 0;
    const bool console = GetConsoleMode(output, &mode) != FALSE;
    std::wstring safe;
    safe.reserve(message.size());
    for (auto c : message) {
        if (c == L'\n') safe += console ? L"\r\n" : L"\n";
        else safe += ((c < 32 && c != L'\t') || c == 127) ? L' ' : c;
    }
    const auto bytes = console ? std::string{} : utf8_text(safe);
    const auto length = console ? safe.size() : bytes.size();
    size_t offset = 0;
    while (offset < length) {
        DWORD written = 0;
        const auto size = static_cast<DWORD>(std::min<size_t>(length - offset, 16384));
        const bool success = console ? WriteConsoleW(output, safe.data() + offset, size, &written, nullptr)
                                     : WriteFile(output, bytes.data() + offset, size, &written, nullptr);
        if (!success || !written) throw std::runtime_error(tr(Msg::OutputFailed, {std::to_string(GetLastError())}));
        offset += written;
    }
}
void write_log(const char* tag, const std::string& message) {
    auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm time{};
    localtime_s(&time, &timestamp);
    std::string safe = message;
    for (auto& character : safe) if (static_cast<unsigned char>(character) < 32 || character == 127) character = ' ';
    std::ostringstream record;
    record << std::put_time(&time, "%Y-%m-%d %H:%M:%S") << " [" << tag << "] " << safe << '\n';
    std::lock_guard<std::mutex> guard(outputMutex);
    // A closed output pipe must not turn a networking callback into an exception.
    try { write_output(STD_ERROR_HANDLE, unicode_text(record.str())); } catch (...) {}
}
}
void set_log_level(const std::string& value) { level = value == "error" ? 0 : (value == "debug" ? 2 : 1); }
void log_info(const std::string& value) { if (level >= 1) write_log("INFO", value); }
void log_error(const std::string& value) { write_log("ERROR", value); }
void log_debug(const std::string& value) { if (level >= 2) write_log("DEBUG", value); }
void write_console_message(const std::wstring& message) {
    std::lock_guard<std::mutex> guard(outputMutex);
    write_output(STD_OUTPUT_HANDLE, message);
}
void write_stdout_message(const std::string& message) { write_console_message(unicode_text(message)); }
void write_stderr_message(const std::string& message) {
    std::lock_guard<std::mutex> guard(outputMutex);
    write_output(STD_ERROR_HANDLE, unicode_text(message));
}
}
