# ProcSocks

Windows 10/11 x64 进程代理。使用 NetFilter SDK WFP 驱动，将指定程序及其后代进程的 TCP/UDP 流量转发到一个 SOCKS5 上游。双击进入中英双语控制台菜单（TUI），也兼容原有命令行；采用 JSON 配置，无需第三方界面控件。按进程接管流量，不使用 TUN、虚拟网卡或全局路由接管。

[English guide](README.en.md)

## 平台状态

| 平台 | 当前状态 | 实现与设计入口 |
| --- | --- | --- |
| Windows 10/11 x64 | 已实现；真实 UAC 与驱动验收单独记录 | [Windows 说明](platforms/windows/README.md)，现有 `src/` 和 `scripts/build.ps1` |
| macOS | 未实现；仅设计占位，技术路线待定 | [macOS 说明](platforms/macos/README.md) |
| Linux | 未实现；仅设计占位，技术路线待定 | [Linux 说明](platforms/linux/README.md) |

当前发布包只支持 Windows。后续各平台独立实现网络接管、进程跟踪和权限管理；配置约定、语言资源及 SOCKS5 协议逻辑可按需要复用。本轮没有迁移 Windows 源码、添加通用网络抽象或其他平台的构建入口。

## 构建

需要 Windows x64 MinGW-w64 GCC（本机验证版本为 15.2.0）和 PowerShell。无需 Visual Studio、Microsoft Windows SDK、CMake 或 ProtocolFilters。

将 `nfsdk-demo-1.7.8.4.zip` 放在项目根目录，在该目录执行：

```powershell
.\scripts\build.ps1
# 编译并运行自动测试：
.\scripts\build.ps1 -Test
# 只编译程序和测试，不执行测试：
.\scripts\build.ps1 -BuildTests
# 编译器不在 PATH 时：
.\scripts\build.ps1 -Compiler 'D:\app\_scoop\apps\gcc\current\bin\g++.exe'
```

构建脚本使用 C++17 和 NetFilter **C API**，配套采用 `release_c_api/x64` 的 `.lib` 与 `nfapi.dll`，不能替换成 C++ API DLL。nlohmann/json 3.12.0 已附带并校验 SHA-256；构建不需要联网。GCC 运行库静态链接，脚本检查动态依赖。

`dist/ProcSocks.exe` 默认剥离调试符号以减小体积；完整调试版本保存在 `build/symbols/ProcSocks.debug.exe`，无需随程序分发。两者使用相同的优化代码和静态运行库。

输出在 `dist`：`ProcSocks.exe`、`nfapi.dll`、`driver/procsocks_nf.sys`、配置示例与说明。整个目录一起使用。SDK 解压目录、构建产物、原始压缩包与私人配置均被 Git 忽略。

## 双击启动（推荐）

1. 保留完整 `dist` 目录，双击 `ProcSocks.exe`。
2. 在 Windows 管理员授权弹窗中选择“是”，进入菜单，无需手动打开管理员终端。语言由配置或系统界面语言决定。取消授权会保留提示，按键后退出。
3. 首次缺少 `config.json` 时，会从同目录示例生成。选择“编辑配置”在记事本中修改代理和目标进程；保存后回到菜单。
4. 选择“检查配置”，然后“启动代理”。驱动未安装时会询问是否安装并启动；确认即可。已安装但停止的驱动会显式启动。
5. 出现“代理已启动”后，再启动或重启目标程序。

菜单支持方向键 + Enter，也可直接按数字：启动代理、检查配置、编辑配置、安装驱动、卸载驱动、退出。界面显示配置路径、代理地址、规则数量、继承/旁路开关和驱动状态，不显示账号密码。

运行中显示滚动日志。按 **Esc 或 Ctrl+C** 停止代理并返回菜单；在菜单中按 Esc 或 Ctrl+C 则退出。关闭窗口会请求停止并退出。停止后可再次启动，每次都会重新读取 JSON；编辑文件不会改变正在运行的代理会话。语言在程序启动时固定，修改 `language` 后需要退出整个程序再启动。卸载驱动需要在菜单中确认。

TUI 默认读取 **EXE 同目录**的 `config.json`，不会覆盖已有文件。也可从交互终端使用：

```powershell
.\ProcSocks.exe tui
.\ProcSocks.exe tui --config 'C:\My Configs\proxy.json'
```

显式指定的配置不会自动生成；相对路径以调用终端的当前目录解析，并在提权前固定为绝对路径。只在交互控制台中启用 TUI；输入或输出被重定向时，无参数启动显示帮助并返回 2，显式 `tui` 返回错误，均不会弹 UAC 或等待键盘。

## 原有命令行

打开**管理员 PowerShell**，进入 `dist`，复制并编辑配置：

```powershell
Copy-Item .\config.example.json .\config.json
notepad .\config.json
.\ProcSocks.exe check --config .\config.json
.\ProcSocks.exe install-driver
.\ProcSocks.exe run --config .\config.json
```

看到 `READY` 后启动或重启目标程序。按 Ctrl+C 停止并退出。配置修改后需要重新执行 `run`。

这些 CLI 命令不自动提权，不增加交互等待。`check` 无需管理员权限，不启动 ETW、不安装或加载驱动。`run`、`install-driver`、`uninstall-driver` 仍要求管理员终端；CLI 的相对配置路径仍以调用者当前目录为准。驱动安装不会立即代理网络；只有启动代理并启用规则后才开始接管新连接。

退出 ProcSocks 后卸载：

```powershell
.\ProcSocks.exe uninstall-driver
```

驱动服务名固定为 `procsocks_nf`，文件位于 Windows 系统驱动目录；不会替换 Netch 等应用的 `netfilter2` 驱动。安装拒绝覆盖内容不同的已有驱动，同名服务路径不符时也拒绝修改。卸载会报告是否需要重启；如提示重启，请先重启再安装。应用和安装/卸载命令共用单实例锁。

## 配置

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

| 字段 | 含义 |
| --- | --- |
| `language` | `auto`（默认）、`zh-CN` 或 `en-US`；省略等同 `auto`，读取当前用户的 Windows 界面语言；中文环境使用简体中文，其他环境回退英文 |
| `proxy.host` / `port` | SOCKS5 IP 或域名、1–65535 端口；IPv6 使用 `::1` 形式，支持 `%12` 数字 scope，不带方括号、端口或 URL scheme |
| `proxy.username` / `password` | 可选，必须同时提供；各 1–255 个 UTF-8 字节，启用后不接受无认证降级 |
| `processes` | 1–256 条精确文件名或绝对路径，不区分大小写；支持中文，不支持通配符与正则 |
| `includeChildren` | 默认 `true`，整个后代进程树继承；`false` 只匹配进程本身 |
| `bypassPrivateNetworks` | 默认 `true`，私有、链路本地、组播及 `255.255.255.255` 地址直连；回环地址始终直连 |
| `logLevel` | `error`、`info`（默认）或 `debug`；debug 显示连接目标与 PID |

用户名密码以普通 JSON 字符串配置，程序不会将其输出到日志；不要提交私人配置。未知字段、重复 JSON 键、空进程列表、非法类型和无效端口会被拒绝。错误日志不回显 JSON 原文。

### 语言与输出

`tui`、`run` 和 `check` 使用各自配置中的 `language`。帮助、版本、安装及卸载命令不读取配置，使用当前用户的系统界面语言；命令名和参数规则保持不变。`auto` 判断的是 Windows 界面语言，不是终端代码页或地区日期格式。简体、繁体等中文界面语言均选择简体中文消息。

配置是有效 JSON 且 `language` 有效时，即使其他字段错误，也使用所选语言报告错误。配置不可读、JSON 损坏、包含重复键或 `language` 无效时，启动语言回退系统语言。TUI 启动后重新检查或加载配置不会切换语言，必须退出并重新启动。

TUI 在请求管理员权限前确定显示语言，并将已确定的语言传给管理员进程；即使 UAC 使用另一个管理员账号，菜单与应用提示也保持原语言。Windows 自身的授权窗口、记事本等系统界面仍由 Windows 管理。

菜单、帮助、配置检查、提权提示、驱动操作及运行错误使用内置中英消息目录。命令名、JSON 键、`READY`、日志级别（`INFO` / `ERROR` / `DEBUG`）和系统错误码保持稳定。原生控制台通过 Unicode API 输出，重定向时输出 UTF-8；帮助、版本和成功的配置检查写入 stdout，运行日志及 CLI 错误写入 stderr，TUI 状态提示在菜单控制台显示。输出使用同一互斥锁，避免多线程消息交错。

## 进程继承与网络行为

- 文件名匹配有边界：`app.exe` 不会匹配 `otherapp.exe`；绝对路径会区分不同目录中的同名程序。
- 使用 ETW 生命周期事件及进程快照，按 PID 和创建时间识别进程实例。已继承进程在父进程退出后保持代理，继续创建的后代也会继承。
- 启动时纳入仍能确认祖先链的已有进程。启动前祖先已退出且没有历史记录的进程无法自动追溯，需要将子程序直接列入配置或重新启动启动器。
- 继承以 Windows 进程树为准。通过已有系统服务、COM broker 等代为创建且不属于该树的程序，需要单独配置。
- 运行中身份/祖先无法确认的流量会短暂等待，仍不能确认则阻断并记录错误；不会猜测继承。受保护进程或事件丢失可能导致这种情况。
- TCP 先完成上游 SOCKS5 握手，再放行原连接至专用本地转发入口；支持半关闭及有界缓冲。只接管规则启用后的新连接，不迁移已有 TCP 连接。
- UDP 要求上游支持 `UDP ASSOCIATE`；按原 socket 保持控制连接，使用实际返回的 relay 地址，只接受该 relay 的响应。SOCKS5 UDP 分片不支持。
- 上游不可达、认证失败、UDP 不支持、握手超时等情况都不会自动直连。UDP association 失败后，该原 socket 后续流量继续阻断；需要目标程序重建 socket（必要时重启程序）后重试。
- 未匹配进程直连；工具自身始终旁路。默认直连 IPv4 RFC1918、IPv6 ULA、链路本地及组播/广播，不将 CGNAT `100.64.0.0/10` 当作私网。
- 系统 DNS 设置保持原样；共享 DNS 服务代发的查询不会因某个应用命中而自动代理。应用自己发起的查询遵循普通进程/地址规则，不保证 DNS 全部走代理。

当前上限：256 个 TCP 代理会话、128 个 UDP 代理会话，另预留 64/32 个分类工作线程。已确认直连的进程不占代理配额；归属仍未知且分类资源耗尽的流量会阻断或丢弃。UDP 最多记录 16,384 个 endpoint；每个队列最多 64 个数据报、256 KiB，每个 association 最多 256 个目标地址、256 KiB 回复元数据。超限会记录错误。TCP 空闲超过 30 分钟会关闭，SOCKS 连接与握手期限为 10 秒，本机重定向连接接受期限为 5 秒。UDP 不会退回原目标直连。

## 测试

```powershell
.\scripts\build.ps1 -Test
# 已构建测试后只运行：
.\scripts\test.ps1 -NoBuild
```

自动测试包含配置校验、语言默认值与显式覆盖、系统语言回退、消息目录完整性及参数占位符一致性、Unicode 与重定向 UTF-8、错误消息脱敏、精确进程匹配、递归继承、父退出与 PID 复用、实际 Windows 进程元数据交叉校验，以及本地模拟 SOCKS5 服务上的认证、逐字节响应、粘包、IPv4/IPv6、UDP、超时与取消。转发引擎测试以模拟 SDK 回调验证数据转发、半关闭、UDP 回注、错误上游、停止清理及并发回调。

TUI 测试覆盖入口分派、提权参数引用和语言上下文、中文/空格路径、默认配置不覆盖、菜单按键、安装/卸载确认、失败重试、配置重载、初始化取消及反复启停。隐藏控制台测试检查原生中文输出、按键事件与控制台模式恢复，使用模拟会话，不弹 UAC。所有测试都不安装驱动、不修改服务状态或打开编辑器；网络测试只使用本机回环网络，受限沙箱若禁止套接字，需要在普通终端运行。

真实驱动验收需要管理员权限和可控的 SOCKS5 上游。建议依次验证：

1. 双击验证 UAC 允许/取消、中英菜单、首次配置、记事本编辑、安装并启动；使用不同界面语言的管理员账号完成 UAC，确认应用语言保持不变；原 CLI `check`/`run` 也正常。
2. 目标程序的新 TCP 连接在上游日志出现，非目标程序保持直连；确认实际出口。
3. 启动器启动不同文件名的子程序、子程序再启动孙程序；父进程退出后，子孙的新 TCP/UDP 连接仍出现在上游。
4. 分别验证 IPv4、IPv6 和 UDP echo；检查响应回到正确进程/socket。
5. 停止上游或使用错误密码，目标连接失败且原目标没有收到直连请求。
6. 验证默认局域网直连、并发连接、反复启动/停止、Esc/Ctrl+C 返回菜单、窗口关闭和重复实例提示；最后卸载驱动。

**编译通过、模拟服务测试通过，不等于真实驱动拦截已验收。** 本次构建、自动测试和待验收事项见 [TEST_RESULTS.md](TEST_RESULTS.md)。

## Demo 与故障排查

当前驱动来自用户提供的 NetFilter SDK Demo，存在过滤连接/UDP socket 数量上限；达到后需要重启 Windows 才能恢复。持续使用需要合适的非 Demo SDK 版本，详见 [官方说明](https://netfiltersdk.com/download.html)。不会尝试绕过 Demo 限制。

- 提示需要管理员权限 / `Administrator privileges required`：换管理员终端运行。
- `nf_init failed`：先执行 `install-driver`，确认 `procsocks_nf` 服务启动、没有另一实例占用；查看 Windows 错误/驱动签名策略。
- 上游只支持 TCP 时，UDP 会失败；应换支持 UDP ASSOCIATE 的 SOCKS5 上游。
- 异常终止留下 ETW 会话时，确认所有 ProcSocks 已退出，再在管理员终端执行 `logman stop ProcSocks.ProcessTracking -ets`，然后重新启动。
- “无法确定进程归属”时可开启 debug 日志，优先将真实联网程序的完整路径直接加入配置并重新启动目标应用。

ProcSocks 退出、崩溃或驱动失效后不提供持续断网保护；退出后新的连接可能恢复直连。Ctrl+C 会主动关闭转发连接并清理规则。无 GUI、Windows 服务、多代理、热更新、HTTP CONNECT 或全局 DNS 接管。
