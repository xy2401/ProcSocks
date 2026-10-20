#pragma once
#include "l10n.hpp"
#include <windows.h>
#include <stdexcept>
namespace ps {
class ControllerError : public std::runtime_error {
public:
    explicit ControllerError(bool busy)
        : std::runtime_error(tr(busy ? Msg::ControllerBusy : Msg::ControllerLockFailed)), busy_(busy) {}
    bool already_running() const { return busy_; }
private:
    bool busy_;
};
class ControllerLock {
public:
    ControllerLock() {
        handle_ = CreateMutexW(nullptr, FALSE, L"Global\\ProcSocks.Controller");
        const auto error = GetLastError();
        if (!handle_ || error == ERROR_ALREADY_EXISTS) {
            if (handle_) CloseHandle(handle_);
            handle_ = nullptr;
            throw ControllerError(error == ERROR_ALREADY_EXISTS);
        }
    }
    ~ControllerLock() { if (handle_) CloseHandle(handle_); }
    ControllerLock(const ControllerLock&) = delete;
    ControllerLock& operator=(const ControllerLock&) = delete;
private:
    HANDLE handle_ = nullptr;
};
}
