#pragma once
#include <string>
namespace ps {
void set_log_level(const std::string& level);
void log_info(const std::string& message);
void log_error(const std::string& message);
void log_debug(const std::string& message);
// Serialize Unicode UI messages with complete background log records.
void write_console_message(const std::wstring& message);
void write_stdout_message(const std::string& message);
void write_stderr_message(const std::string& message);
}
