#pragma once
#include "config.hpp"
#include "process_tracker.hpp"
#include <memory>
namespace ps {
inline constexpr const char* DriverName = "procsocks_nf";
inline constexpr const wchar_t* DriverNameW = L"procsocks_nf";
class Engine {
public:
    Engine(const Config& config, ProcessTracker& tracker);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    void start();
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
