# Windows

**状态：已实现。** 当前程序与发布包面向 Windows 10/11 x64，实际 UAC、驱动拦截及出口验收结果见项目根目录 [TEST_RESULTS.md](../../TEST_RESULTS.md)。编译和模拟测试通过不代表实际驱动验收通过。

## 当前实现

- 网络接管：NetFilter SDK WFP C API，按进程分类 TCP/UDP；不用 TUN、虚拟网卡或全局路由接管。
- 进程跟踪：ETW 生命周期事件结合初始快照，支持递归后代继承和 PID 复用隔离。
- 权限与生命周期：Windows 管理员权限、TUI `runas` 提权、独立驱动服务 `procsocks_nf`、单实例锁和控制台取消清理。
- 交互与语言：原生 Windows Unicode 控制台、中英消息目录、JSON `language` 配置；重定向输出 UTF-8。
- 转发协议：一个 SOCKS5 上游，TCP CONNECT 与 UDP ASSOCIATE，支持 IPv4/IPv6；代理故障时对应流量不回退直连。

现有源码保留在项目根目录 `src/`；构建入口为 `scripts/build.ps1`，测试入口为 `scripts/test.ps1`。使用 C++17、MinGW-w64 GCC 和 PowerShell，不引入 CMake。发布包位于 `dist/`，包括可执行程序、NetFilter DLL、驱动和说明。

本目录仅记录 Windows 的职责与平台边界，没有将现有源码移入此处。配置约定、`src/messages/` 消息目录及 SOCKS5 协议逻辑可供未来平台参考；现有实现中的 Windows API 调用仍需分别处理，不能直接声称其代码可跨平台编译。

## 验收要求

保留配置与语言、协议、进程继承、驱动保护、代理失败阻断、取消清理和重复启停自动测试。真实 Windows 验收另行覆盖：

1. 双击启动、UAC 允许/取消、跨管理员账号的语言保持和首次配置创建。
2. 中英菜单、中文/空格配置路径、记事本编辑、非交互退出和重复实例提示。
3. 实际目标进程及其后代的 TCP/UDP IPv4/IPv6 出口、局域网旁路、代理断线无直连。
4. 驱动安装/启动/卸载、Esc/Ctrl+C 停止返回、窗口关闭及重复启停清理。

Demo 数量限制、系统 DNS 保留、已有 TCP 不接管、启动前已失去祖先信息的边界，以及程序退出或崩溃后不提供持续断网保护，见根目录 [使用说明](../../README.md)。
