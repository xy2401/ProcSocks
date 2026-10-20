#include "l10n.hpp"
// Exercise the internal parser with synthetic data only. No install/uninstall or
// registry write function is called; build.ps1 excludes driver.o from unit tests.
#include "../src/driver.cpp"
#include <iostream>
#include <initializer_list>

namespace {
std::vector<wchar_t> operations(std::initializer_list<std::pair<std::wstring, std::wstring>> pairs) {
    std::vector<wchar_t> result;
    for (const auto& pair : pairs) {
        result.insert(result.end(), pair.first.begin(), pair.first.end()); result.push_back(0);
        result.insert(result.end(), pair.second.begin(), pair.second.end()); result.push_back(0);
    }
    result.push_back(0);
    return result;
}
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
    ps::set_language(ps::Language::EnUS);
    try {
        const std::filesystem::path expected = L"C:\\Windows\\System32\\drivers\\procsocks_nf.sys";
        const std::wstring windows = L"C:\\Windows";
        const auto touches = [&](const std::vector<wchar_t>& data) { return ps::pending_operations_touch_path(data, expected, windows); };
        check(!touches({}), "empty registry data");
        check(!touches(operations({})), "empty list");
        check(touches(operations({{L"\\??\\C:\\WINDOWS\\SYSTEM32\\DRIVERS\\PROCSOCKS_NF.SYS", L""}})), "native prefix and case-insensitive deletion");
        check(touches(operations({{L"\\\\?\\C:\\Windows\\System32\\drivers\\procsocks_nf.sys", L""}})), "extended DOS prefix deletion");
        check(touches(operations({{L"\\SystemRoot\\System32\\drivers\\procsocks_nf.sys", L""}})), "SystemRoot deletion");
        check(touches(operations({{L"\\DosDevices\\C:\\Windows\\System32\\drivers\\procsocks_nf.sys", L""}})), "DosDevices deletion");
        check(touches(operations({{L"\\??\\C:\\temp\\new.sys", L"!\\??\\C:\\Windows\\System32\\drivers\\procsocks_nf.sys"}})), "replacement destination");
        check(touches(operations({{expected.wstring(), L"C:\\temp\\old.sys"}})), "move away from driver path");
        check(touches(operations({{L"C:\\temp\\other.tmp", L""}, {expected.wstring(), L""}})), "deletion after unrelated empty destination");
        check(!touches(operations({{L"C:\\temp\\procsocks_nf.sys", L""}, {L"C:\\temp\\new.sys", L"C:\\temp\\renamed.sys"}})), "same basename elsewhere does not block");
        check(!touches(operations({{expected.wstring() + L".bak", L""}})), "driver path must match exactly");
        check(touches(operations({{L"C:/Windows/System32/drivers/../drivers/procsocks_nf.sys", L""}})), "path normalization");
        bool malformed = false;
        try { touches({L'x'}); } catch (const std::exception&) { malformed = true; }
        check(malformed, "unterminated source must fail safely");
        malformed = false;
        try { touches({L'x', 0}); } catch (const std::exception&) { malformed = true; }
        check(malformed, "missing destination must fail safely");
        std::cout << "driver_guard_tests: 14 checks passed; synthetic registry data only\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "driver_guard_tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
