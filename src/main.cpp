#include "net.hpp"
#include "config.hpp"
#include "controller.hpp"
#include "driver.hpp"
#include "launch.hpp"
#include "log.hpp"
#include "session.hpp"
#include "tui.hpp"
#include <stdexcept>

namespace {
void usage() {
    ps::write_stdout_message(ps::tr(ps::Msg::Usage));
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        ps::set_language(ps::system_language());
        const std::vector<std::wstring> args(argv + 1, argv + argc);
        const bool interactive = ps::interactive_console();
        const auto options = ps::parse_launch(args, interactive, ps::executable_path(), std::filesystem::current_path());
        if (options.command == ps::Command::Help) { usage(); return options.helpExitCode; }
        if (options.command == ps::Command::Invalid) { usage(); return 2; }
        if (options.command == ps::Command::Version) { ps::write_stdout_message("ProcSocks 0.1.0\n"); return 0; }
        if (options.command == ps::Command::Tui || options.command == ps::Command::Run || options.command == ps::Command::Check)
            ps::set_language(options.uiLanguage != ps::Language::Auto ? options.uiLanguage : ps::config_language(options.configPath));
        if (options.command == ps::Command::Tui) {
            // Report this error in the selected config language before creating
            // any console controller or requesting administrator access.
            if (!interactive) throw std::runtime_error(ps::tr(ps::Msg::TuiInteractiveRequired));
            return ps::launch_tui(options);
        }
        if (options.command == ps::Command::Check) {
            const auto config = ps::load_config(options.configPath);
            ps::write_stdout_message(ps::tr(ps::Msg::ConfigurationOk, {
                std::to_string(config.processes.size()), config.includeChildren ? "true" : "false",
                ps::tr(config.proxy.authenticate ? ps::Msg::AuthConfigured : ps::Msg::AuthNone)}));
            return 0;
        }
        if (!ps::is_administrator()) throw std::runtime_error(ps::tr(ps::Msg::AdminRequired));
        ps::ControllerLock instance;
        if (options.command == ps::Command::Install) { ps::install_driver(); return 0; }
        if (options.command == ps::Command::Uninstall) { ps::uninstall_driver(); return 0; }
        ps::ControlSignals signals;
        if (!signals.begin_operation()) return 0;
        ps::run_proxy_session(ps::load_config(options.configPath), signals);
        signals.end_operation();
        return 0;
    } catch (const std::exception& error) {
        ps::log_error(error.what());
        return 1;
    }
}
