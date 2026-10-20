# ProcSocks

A Windows 10/11 x64 process proxy. ProcSocks uses the NetFilter SDK WFP driver to send selected processes' TCP/UDP traffic, including inherited descendants, through one SOCKS5 server. It does not use TUN, a virtual network adapter, or global route interception. Double-click the executable for the native console menu, or use the CLI. Configuration is JSON; no third-party UI library is required.

[中文说明](README.md)

## Platform status

| Platform | Status | Details |
| --- | --- | --- |
| Windows 10/11 x64 | Implemented; real UAC and driver acceptance are recorded separately | [Windows design notes](platforms/windows/README.md) |
| macOS | Not implemented; design placeholder, approach undecided | [macOS design notes](platforms/macos/README.md) |
| Linux | Not implemented; design placeholder, approach undecided | [Linux design notes](platforms/linux/README.md) |

The current release is Windows-only. Future platforms will independently implement traffic interception, process tracking, and privileges. Configuration conventions, message resources, and SOCKS5 protocol logic may be reused where appropriate. The placeholders do not add binaries, build systems, or working proxy support for other platforms.

## Build

Use Windows x64 MinGW-w64 GCC and PowerShell; GCC 15.2.0 is the locally verified compiler. Put `nfsdk-demo-1.7.8.4.zip` in the project root, then run:

```powershell
.\scripts\build.ps1
# Build and run automated tests:
.\scripts\build.ps1 -Test
# Build application and tests without running tests:
.\scripts\build.ps1 -BuildTests
```

The build uses C++17, the NetFilter C API, and the SDK's `release_c_api/x64` import library and DLL. Do not substitute the C++ API DLL. nlohmann/json 3.12.0 is included and checksum-verified. No Visual Studio, CMake, ProtocolFilters, or network download is required.

Use the complete `dist` directory: `ProcSocks.exe`, `nfapi.dll`, `driver/procsocks_nf.sys`, configuration example, licenses, and documentation. GCC runtimes are statically linked. The release executable has debug symbols stripped; the matching full-symbol executable is kept in `build/symbols/ProcSocks.debug.exe` outside the distribution. Private configuration, SDK archives and extraction, and build outputs are ignored by Git.

## Double-click and TUI

1. Double-click `ProcSocks.exe` and accept Windows administrator authorization. Cancelling UAC keeps a message visible until a key is pressed.
2. If the default `config.json` is missing, ProcSocks copies the example beside the executable. Existing files are never overwritten, and the proxy does not start automatically.
3. Choose **Edit configuration** to open Notepad. Save your proxy address and target process rules, then choose **Check configuration** and **Start proxy**.
4. If the driver is absent, confirm installation and startup. An installed but stopped driver is started explicitly.
5. Start or restart target applications after the proxy reports that it is running.

Use arrow keys and Enter, or number shortcuts. The menu offers start, check, edit, install driver, uninstall driver, and exit. It shows the configuration path, proxy address, rule count, inheritance/bypass settings, and driver state; it never shows authentication credentials.

During a session, Esc or Ctrl+C stops the proxy, completes cleanup, and returns to the menu. Esc or Ctrl+C in the menu exits. Closing the console requests shutdown and exit. Each new session reloads the proxy configuration and creates fresh tracking and forwarding objects. Language is fixed for the entire program run; exit and restart the application after changing `language`.

```powershell
.\ProcSocks.exe tui
.\ProcSocks.exe tui --config 'C:\My Configs\proxy.json'
```

The default TUI configuration is `config.json` beside the executable. An explicitly supplied relative path is resolved against the caller's working directory before elevation. Explicit configuration files are not automatically created. TUI requires interactive console input and output. Without a console, no-argument startup prints help and exits with code 2; explicit `tui` reports an error. Neither case waits for input or requests UAC.

## CLI

`check` does not require administrator privileges or load the driver. Run driver commands and `run` from an administrator terminal:

```powershell
Copy-Item .\config.example.json .\config.json
notepad .\config.json
.\ProcSocks.exe check --config .\config.json
.\ProcSocks.exe install-driver
.\ProcSocks.exe run --config .\config.json
# After stopping ProcSocks:
.\ProcSocks.exe uninstall-driver
```

Start target applications after `READY`. Ctrl+C stops and exits. CLI commands do not automatically elevate or wait for interactive confirmation. CLI configuration paths remain relative to the caller's working directory, with `config.json` as the default. Driver operations and the application share a single-instance lock.

The dedicated driver service is `procsocks_nf`; it does not replace another application's `netfilter2` service. Conflicting driver contents or service paths are rejected. Follow any restart requirement reported during uninstall.

## Configuration and language

```json
{
  "language": "auto",
  "proxy": {
    "host": "127.0.0.1",
    "port": 1080
  },
  "processes": [
    "launcher.exe",
    "C:\\Apps\\Example\\client.exe"
  ],
  "includeChildren": true,
  "bypassPrivateNetworks": true,
  "logLevel": "info"
}
```

| Setting | Meaning |
| --- | --- |
| `language` | `auto`, `zh-CN`, or `en-US`. Omitted means `auto`: Chinese Windows user UI languages select Simplified Chinese; all other languages select English. |
| `proxy.host` / `port` | SOCKS5 IP address or hostname and integer port 1–65535. Use IPv6 without brackets, scheme, or port; numeric scope IDs such as `%12` are supported. |
| `proxy.username` / `password` | Optional, supplied together; each must be 1–255 UTF-8 bytes. Configured authentication does not downgrade to unauthenticated SOCKS5. |
| `processes` | 1–256 exact filenames or absolute paths, matched case-insensitively. Unicode is supported; wildcards and regular expressions are not. |
| `includeChildren` | Defaults to `true`; descendants inherit proxy selection. `false` matches only each process itself. |
| `bypassPrivateNetworks` | Defaults to `true`; private, link-local, multicast, and limited broadcast destinations go direct. Loopback always goes direct. |
| `logLevel` | `error`, `info` (default), or `debug`. Debug logging includes connection destinations and PIDs. |

`tui`, `run`, and `check` read their configuration's language. Help, version, install, and uninstall commands do not read configuration and use the current user's Windows UI language. This is the interface language, not the terminal code page or regional date format.

A valid language in valid JSON controls diagnostics for other invalid fields. Unreadable files, malformed JSON, duplicate keys, and invalid language values fall back to the system language at startup. Reloading configuration in TUI does not change the selected language. Before elevation, TUI resolves the language and passes it to the administrator process, preserving application messages even when UAC uses another administrator account. Windows' own UAC dialog and Notepad retain their system-managed language.

App-owned menu text, help, configuration checks, elevation prompts, driver messages, logs, and proxy errors have English and Simplified Chinese resources. Command names, JSON keys, `READY`, log levels, and system error codes stay stable. Native consoles receive Unicode output; redirected output is UTF-8. Help, version, and successful configuration checks use stdout; logs and CLI errors use stderr. TUI status messages appear in the menu console. Output is synchronized across threads.

Credentials are stored as ordinary JSON strings and are never printed. Do not commit private configuration. Unknown fields, duplicate keys, empty process lists, invalid types, and invalid ports are rejected without echoing raw JSON.

## Behavior and limits

- Descendants are tracked using ETW and process snapshots with creation-time checks against PID reuse. Inherited selection survives parent exit. Ancestors that exited before tracking began cannot always be recovered; configure the actual child or restart the launcher. Processes created through an existing broker/service may need their own rules.
- Only new TCP connections are intercepted. UDP requires SOCKS5 `UDP ASSOCIATE`; the control connection remains open, and responses must come from the selected relay. SOCKS5 UDP fragmentation is unsupported.
- Proxy connection failures, authentication failures, and unsupported UDP do not fall back to direct traffic. A failed UDP association keeps blocking that original socket; the application must recreate it to retry.
- Unmatched processes and ProcSocks itself bypass the proxy. Private-network bypass covers RFC1918 and IPv6 ULA, not CGNAT `100.64.0.0/10`.
- System DNS settings are unchanged. Shared DNS services do not automatically inherit another application's selection; complete DNS proxying is not promised.
- The current limits include 256 TCP proxy sessions and 128 UDP proxy sessions with bounded queues and classification resources. See the [Chinese reference](README.md) for detailed bounds and timeouts.
- NetFilter SDK Demo has filtering quotas; reaching them requires restarting Windows. The application does not bypass Demo restrictions.
- Persistent blocking after application exit, crashes, or driver failure is outside the current scope. New connections may go direct after exit.

## Verification

Run `.\scripts\test.ps1 -NoBuild` after building the tests. Automated coverage includes language selection/fallback, catalog completeness and matching placeholders, credential redaction, Unicode console output, UTF-8 redirection, configuration paths and creation, elevation arguments and language context, menu retry/cancellation/restart, process inheritance, driver protection, and SOCKS5/forwarding behavior against local test doubles.

Automated tests do not install drivers, change driver services, open an editor, or request real UAC. Loopback network tests may require an ordinary terminal when a restricted sandbox blocks sockets.

Real double-click UAC allow/cancel, switching to an administrator account with a different UI language, Notepad editing, and actual driver/network interception require separate Windows acceptance. Confirm target-process IPv4/IPv6 TCP/UDP egress, child inheritance, private-network bypass, no direct fallback on upstream failure, and cleanup during stop/window-close. Build and simulated-test success do not establish real-driver success. See [TEST_RESULTS.md](TEST_RESULTS.md) for the recorded results and outstanding manual checks.
