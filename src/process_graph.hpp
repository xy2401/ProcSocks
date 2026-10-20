#pragma once
#include "process_tracker.hpp"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ps {
// Read-only diagnostic used by the test executable; never starts ETW.
bool verify_process_metadata(std::string& details);
// Times are Windows FILETIME ticks. A PID by itself never identifies a process.
struct ProcessIdentity {
    std::uint32_t pid = 0;
    std::uint64_t created = 0;
    bool operator<(const ProcessIdentity& other) const {
        return pid < other.pid || (pid == other.pid && created < other.created);
    }
    bool operator==(const ProcessIdentity& other) const {
        return pid == other.pid && created == other.created;
    }
};
struct ProcessObservation {
    ProcessIdentity identity;
    std::uint32_t parentPid = 0;
    std::wstring image;
    bool fullImagePath = false;
    // Only set after verifying the parent's creation time against the child.
    std::optional<ProcessIdentity> verifiedParent;
    bool ancestryBoundary = false;
};

// Caller provides synchronization. Kept separate from Windows event collection
// so ordering, exit and PID-reuse cases can be tested deterministically.
class ProcessGraph {
public:
    ProcessGraph(std::vector<std::wstring> patterns, bool includeChildren);
    void observe(const ProcessObservation& observation);
    void exited(ProcessIdentity identity, std::uint64_t exitTime);
    ProcessDecision classify(ProcessIdentity identity);
    void prune(std::uint64_t exitedBefore);
    std::size_t size() const { return nodes_.size(); }
    static bool image_matches(const std::wstring& pattern, const std::wstring& image);
private:
    struct Node {
        ProcessObservation value;
        std::uint64_t exited = 0;
        bool proxy = false;
    };
    std::optional<ProcessIdentity> parent_of(const Node& node) const;
    ProcessDecision decide(ProcessIdentity identity, std::set<ProcessIdentity>& visiting);
    std::vector<std::wstring> patterns_;
    bool includeChildren_;
    std::map<ProcessIdentity, Node> nodes_;
};
}
