#pragma once
namespace ps {
enum class DriverState { NotInstalled, Stopped, Running, Pending };
bool is_administrator();
// Read-only; throws if SCM access fails or this service belongs to another path/type.
DriverState query_driver_state();
// Starts only an already-installed, verified service. Never installs/registers it.
void start_driver();
void install_driver();
void uninstall_driver();
}
