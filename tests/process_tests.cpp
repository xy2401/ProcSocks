#include "process_graph.hpp"
#include <iostream>
#include <stdexcept>

using ps::ProcessDecision;
using ps::ProcessGraph;
using ps::ProcessIdentity;
using ps::ProcessObservation;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
ProcessObservation process(unsigned pid, unsigned long long time, unsigned parent, const wchar_t* path,
    std::optional<ProcessIdentity> verified = {}, bool boundary = false) {
    return {{pid,time},parent,path,path[0] && path[1] == L':',verified,boundary};
}
void matching() {
    require(ProcessGraph::image_matches(L"CLIENT.EXE", L"C:\\Apps\\client.exe"), "Case-insensitive filename");
    require(!ProcessGraph::image_matches(L"client.exe", L"C:\\Apps\\myclient.exe"), "Exact filename");
    require(ProcessGraph::image_matches(L"C:\\Apps\\Client.exe", L"\\\\?\\C:\\apps\\client.exe"), "Extended path");
    require(ProcessGraph::image_matches(L"\\\\server\\share\\client.exe", L"\\\\?\\UNC\\server\\share\\client.exe"), "Extended UNC");
    require(ProcessGraph::image_matches(L"\\\\server\\share\\client.exe", L"\\\\?\\unc\\server\\share\\client.exe"), "Case-insensitive UNC prefix");
    require(ProcessGraph::image_matches(L"C:\\工具\\启动.exe", L"c:\\工具\\启动.exe"), "Unicode path");
    require(!ProcessGraph::image_matches(L"C:\\Apps\\client.exe", L"D:\\Apps\\client.exe"), "Full path must match");
}
void recursive_and_exit() {
    ProcessGraph graph({L"launcher.exe"}, true);
    graph.observe(process(10,100,0,L"C:\\launcher.exe"));
    graph.observe(process(20,110,10,L"C:\\helper.exe", ProcessIdentity{10,100}));
    graph.observe(process(30,120,20,L"C:\\worker.exe", ProcessIdentity{20,110}));
    require(graph.classify({30,120}) == ProcessDecision::Proxy, "Grandchild inheritance");
    graph.exited({10,100},130);
    graph.exited({20,110},140);
    graph.observe(process(40,150,30,L"C:\\more.exe", ProcessIdentity{30,120}));
    require(graph.classify({40,150}) == ProcessDecision::Proxy, "Inheritance after ancestor exit");
    graph.prune(145);
    require(graph.classify({40,150}) == ProcessDecision::Proxy, "Inheritance survives tombstone cleanup");
}
void event_ordering() {
    ProcessGraph graph({L"launcher.exe"}, true);
    graph.observe(process(20,110,10,L"C:\\child.exe"));
    require(graph.classify({20,110}) == ProcessDecision::Unknown, "Missing parent is uncertain");
    graph.exited({10,100},120);
    graph.observe(process(10,100,0,L"C:\\launcher.exe"));
    require(graph.classify({20,110}) == ProcessDecision::Proxy, "Stop before start / child before parent");
    graph.observe(process(30,130,10,L"C:\\other.exe"));
    require(graph.classify({30,130}) == ProcessDecision::Unknown, "Exited parent cannot own later child");
}
void reuse() {
    ProcessGraph graph({L"launcher.exe"}, true);
    graph.observe(process(10,100,0,L"C:\\launcher.exe"));
    graph.exited({10,100},120);
    graph.observe(process(10,200,0,L"C:\\unrelated.exe"));
    graph.observe(process(20,210,10,L"C:\\child.exe", ProcessIdentity{10,200}));
    require(graph.classify({10,200}) == ProcessDecision::Direct, "Reused root PID is not selected");
    require(graph.classify({20,210}) == ProcessDecision::Direct, "Reused parent PID doesn't inherit");
    graph.observe(process(20,300,0,L"C:\\launcher.exe"));
    require(graph.classify({20,300}) == ProcessDecision::Proxy, "New matching process selected");
    require(graph.classify({20,210}) == ProcessDecision::Direct, "Different child generations isolated");
    graph.exited({20,210},250);
    require(graph.classify({20,300}) == ProcessDecision::Proxy, "Late old exit doesn't remove newer instance");
}
void missing_and_initial() {
    ProcessGraph graph({L"C:\\Apps\\launcher.exe"}, true);
    graph.observe(process(10,100,999,L"C:\\unrelated.exe", {}, true));
    require(graph.classify({10,100}) == ProcessDecision::Direct, "Initial unrecoverable ancestry boundary");
    graph.observe(process(20,110,999,L"C:\\unrelated.exe"));
    require(graph.classify({20,110}) == ProcessDecision::Unknown, "Runtime orphan cannot bypass");
    graph.observe(process(30,120,0,L"launcher.exe"));
    require(graph.classify({30,120}) == ProcessDecision::Unknown, "Basename can't disprove full path match");
    graph.observe(process(30,120,0,L"C:\\Apps\\launcher.exe"));
    require(graph.classify({30,120}) == ProcessDecision::Proxy, "Late full image resolves root");
    graph.observe(process(40,130,0,L""));
    require(graph.classify({40,130}) == ProcessDecision::Unknown, "Unreadable image is uncertain");
    graph.observe(process(50,140,30,L"", ProcessIdentity{30,120}));
    require(graph.classify({50,140}) == ProcessDecision::Proxy, "Verified inherited identity needs no path");
}
void disabled_and_pruning() {
    ProcessGraph graph({L"launcher.exe"}, false);
    graph.observe(process(10,100,0,L"C:\\launcher.exe"));
    graph.observe(process(20,110,10,L"C:\\child.exe", ProcessIdentity{10,100}));
    require(graph.classify({20,110}) == ProcessDecision::Direct, "includeChildren=false");
    graph.exited({10,100},120);
    graph.prune(130);
    require(graph.classify({20,110}) == ProcessDecision::Direct, "Pruning doesn't enable inheritance");
    ProcessGraph direct({L"launcher.exe"}, true);
    direct.observe(process(10,100,0,L"C:\\unrelated.exe"));
    direct.observe(process(20,110,10,L"C:\\child.exe", ProcessIdentity{10,100}));
    direct.exited({10,100},120);
    direct.prune(130);
    require(direct.classify({20,110}) == ProcessDecision::Direct, "Confirmed direct ancestry survives pruning");
}
void kernel_boundary() {
    ProcessGraph graph({L"C:\\Apps\\launcher.exe"}, true);
    graph.observe(process(100,100,4,L"smss.exe"));
    graph.observe(process(200,110,100,L"C:\\Windows\\System32\\wininit.exe", ProcessIdentity{100,100}));
    graph.observe(process(300,120,200,L"C:\\Windows\\System32\\services.exe", ProcessIdentity{200,110}));
    require(graph.classify({300,120}) == ProcessDecision::Direct, "Known basename excludes target; System is root boundary");
    ProcessGraph ambiguous({L"C:\\Windows\\System32\\smss.exe"}, true);
    ambiguous.observe(process(100,100,4,L"smss.exe"));
    ambiguous.observe(process(200,110,100,L"C:\\Windows\\System32\\wininit.exe", ProcessIdentity{100,100}));
    require(ambiguous.classify({200,110}) == ProcessDecision::Unknown, "Matching basename with unknown full path remains uncertain");
}
}
int main(int argc, char** argv) {
    try {
        matching(); recursive_and_exit(); event_ordering(); reuse(); missing_and_initial(); disabled_and_pruning(); kernel_boundary();
        std::cout << "process tests passed\n";
        if (argc == 2 && std::string(argv[1]) == "--metadata-smoke") {
            std::string details;
            auto verified = ps::verify_process_metadata(details);
            require(verified, details.c_str());
            std::cout << details << '\n';
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "process tests FAILED: " << e.what() << '\n';
        return 1;
    }
}
