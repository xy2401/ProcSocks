#include "process_graph.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>

namespace ps {
namespace {
bool begins(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() && CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()),
        prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}
std::wstring normalize(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (begins(value, L"\\\\?\\UNC\\")) value = L"\\\\" + value.substr(8);
    else if (begins(value, L"\\\\?\\")) value.erase(0, 4);
    return value;
}
bool same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
        static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
}
ProcessGraph::ProcessGraph(std::vector<std::wstring> patterns, bool includeChildren)
    : patterns_(std::move(patterns)), includeChildren_(includeChildren) {}
bool ProcessGraph::image_matches(const std::wstring& pattern, const std::wstring& image) {
    auto p = normalize(pattern), i = normalize(image);
    if (p.find(L'\\') == std::wstring::npos) {
        auto slash = i.find_last_of(L'\\');
        if (slash != std::wstring::npos) i.erase(0, slash + 1);
    }
    return !i.empty() && same(p, i);
}
void ProcessGraph::observe(const ProcessObservation& observation) {
    if (!observation.identity.pid || !observation.identity.created) return;
    auto& node = nodes_[observation.identity];
    // A fallback snapshot must not erase an ETW-derived image or parent link.
    if (!node.value.identity.created) node.value = observation;
    else {
        if (!observation.image.empty() && (observation.fullImagePath || node.value.image.empty())) {
            node.value.image = observation.image;
            node.value.fullImagePath = observation.fullImagePath;
        }
        if (observation.parentPid) node.value.parentPid = observation.parentPid;
        if (observation.verifiedParent) node.value.verifiedParent = observation.verifiedParent;
        node.value.ancestryBoundary |= observation.ancestryBoundary;
    }
    for (const auto& pattern : patterns_) {
        if (image_matches(pattern, node.value.image)) node.proxy = true;
    }
}
void ProcessGraph::exited(ProcessIdentity identity, std::uint64_t exitTime) {
    // Keep tombstones even when the stop arrives before its start event.
    if (!identity.pid || !identity.created || exitTime < identity.created) return;
    auto& node = nodes_[identity];
    node.value.identity = identity;
    node.exited = exitTime;
}
std::optional<ProcessIdentity> ProcessGraph::parent_of(const Node& node) const {
    const auto& child = node.value;
    if (child.verifiedParent) {
        auto it = nodes_.find(*child.verifiedParent);
        if (it != nodes_.end() && it->first.created <= child.identity.created &&
            (!it->second.exited || it->second.exited >= child.identity.created)) return it->first;
        return {};
    }
    // An unverified cached PID could already have been reused. Only an exact
    // closed lifetime is safe to infer; a live parent is verified by the collector.
    std::optional<ProcessIdentity> best;
    auto it = nodes_.lower_bound({child.parentPid, 0});
    for (; it != nodes_.end() && it->first.pid == child.parentPid; ++it) {
        if (it->first.created <= child.identity.created && it->second.exited >= child.identity.created)
            best = it->first;
    }
    return best;
}
ProcessDecision ProcessGraph::decide(ProcessIdentity identity, std::set<ProcessIdentity>& visiting) {
    auto it = nodes_.find(identity);
    if (it == nodes_.end() || visiting.size() >= 256 || !visiting.insert(identity).second) return ProcessDecision::Unknown;
    auto& node = it->second;
    if (node.proxy) return ProcessDecision::Proxy;
    bool ownKnown = !node.value.image.empty();
    if (!node.value.fullImagePath) {
        for (const auto& pattern : patterns_) {
            auto slash = pattern.find_last_of(L"\\/");
            if (slash != std::wstring::npos && image_matches(pattern.substr(slash + 1), node.value.image)) ownKnown = false;
        }
    }
    // System (PID 4) is the Windows kernel process, not an application ancestor.
    // End the process-tree walk there, just as for PID 0.
    if (includeChildren_ && node.value.parentPid != 4) {
        auto parent = parent_of(node);
        if (parent) {
            auto inherited = decide(*parent, visiting);
            if (inherited == ProcessDecision::Proxy) {
                node.proxy = true;
                return ProcessDecision::Proxy;
            }
            if (inherited == ProcessDecision::Unknown) return ProcessDecision::Unknown;
        } else if (!node.value.ancestryBoundary && node.value.parentPid != 0) {
            return ProcessDecision::Unknown;
        }
    }
    return ownKnown ? ProcessDecision::Direct : ProcessDecision::Unknown;
}
ProcessDecision ProcessGraph::classify(ProcessIdentity identity) {
    std::set<ProcessIdentity> visiting;
    return decide(identity, visiting);
}
void ProcessGraph::prune(std::uint64_t exitedBefore) {
    std::set<ProcessIdentity> removable;
    for (const auto& [key, value] : nodes_) {
        if (value.exited && value.exited < exitedBefore) removable.insert(key);
    }
    // Materialize inherited decisions before deleting old parent records. Unknown
    // remains unknown; never make an uncertain orphan direct to save memory.
    for (auto& [key, value] : nodes_) {
        auto parent = parent_of(value);
        if (parent && removable.count(*parent)) {
            auto decision = classify(*parent);
            if (decision == ProcessDecision::Proxy && includeChildren_) value.proxy = true;
            if (decision == ProcessDecision::Direct) value.value.ancestryBoundary = true;
            value.value.verifiedParent.reset();
        }
    }
    for (auto id : removable) nodes_.erase(id);
}
}
