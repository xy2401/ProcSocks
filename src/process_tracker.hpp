#pragma once
#include "config.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
namespace ps {
enum class ProcessDecision { Direct, Proxy, Unknown };
class ProcessTracker {
public:
    explicit ProcessTracker(const Config& config);
    ~ProcessTracker();
    ProcessTracker(const ProcessTracker&) = delete;
    ProcessTracker& operator=(const ProcessTracker&) = delete;
    void start();
    void stop();
    ProcessDecision classify(std::uint32_t pid, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
