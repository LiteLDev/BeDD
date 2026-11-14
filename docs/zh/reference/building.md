# 构建与贡献

## 构建

需要 Windows x64、[xmake](https://xmake.io)、带 C++ 工作负载（MSVC 14.4x 或更新）的 Visual Studio 生成工具（2022 或 2026）、
构建模组用的 LLVM（`clang-cl`）、构建 `BeDD.exe` 用的 .NET 10 SDK，以及构建文档站点用的 Node.js 24 或更新版本。
缺的东西用 winget 装一次（LLVM 和生成工具会要求管理员权限），然后开一个新终端，让 `PATH` 包含它们：

```powershell
winget install -e --id Xmake-io.Xmake
winget install -e --id LLVM.LLVM
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
#   或 Visual Studio 2026：--id Microsoft.VisualStudio.BuildTools，--override 相同
winget install -e --id Microsoft.DotNet.SDK.10
winget install -e --id OpenJS.NodeJS.LTS
```

**模组**（`bin\BeDD\`：DLL 和它的 manifest）。xmake 自己会找到生成工具；LLVM 要在 `PATH` 上：

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
xmake f -P . -y -m release -p windows --target_type=client   # 调试版用 -m debug
xmake -P . -y
```

`-P .` 表示构建当前目录的项目。不加的话 xmake 会向上查找项目，在另一个检出里的 git worktree 中会去构建那个检出。
模组打包器从 `git describe` 读取版本号，所以仓库至少要有一个提交（以及它的标签：浅克隆里先 `git fetch --tags`）。

**服务端目标**（客户端服务端共同调试模式）：上面换成 `--target_type=server`，
构建装了 LeviLamina 的 Bedrock 专用服务器（BDS）运行的模组，输出到 `bin\BeDD-server\`（DLL 和 manifest，平台 `server`）。
它只有控制管道、只用本机网络和服务端操作（`src/ServerTimeScale.cpp`），不含客户端的任何部分（文件由 `xmake.lua` 列出；
`src/` 的其余部分需要客户端版 LeviLamina 的头文件）。把这个目录复制到服务器的 `mods\`（即 `mods\BeDD-server\`）；它读取 `mods\BeDD-server\config\config.json`，提供 `\\.\pipe\BeDD-Server`。

```powershell
git checkout -- xmake-requires.lock
xmake f -P . -y -m release -p windows --target_type=server
xmake -P . -y
git checkout -- xmake-requires.lock
```

`xmake-requires.lock` 锁定两个目标的包，但 xmake 会把它改写成只含当前配置目标的锁定项：构建完、配置另一个目标前，
先 `git checkout -- xmake-requires.lock`（CI 也这样做）。要更新锁定版本，分别配置两个目标，保留两边的条目，提交合并后的文件。

**应用**（`BeDD.exe`，C# NativeAOT）。AOT 链接器通过 `vswhere.exe` 找 C++ 生成工具，它在 Visual Studio Installer 目录里；
发布时把它加到 `PATH`：

```powershell
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
dotnet publish app/BeDD/BeDD.csproj -c Release -o bin\app   # -> bin\app\BeDD.exe
dotnet test app/BeDD.slnx                                   # 单元测试（不需要 vswhere）
```

**安装程序**（WiX Toolset 7，`packaging/wix`，参照 lip）。`Package` 生成按用户安装的 MSI，`Bundle` 生成套在它外面的 `setup.exe`
（标准界面，英文或中文）。两者都取 `bin\release\BeDD` 里的发布布局（`.github/actions/stage` 用 `bin\app`、`bin\BeDD`、`bin\BeDD-server`
和协议规范生成它），版本取 `app/BeDD/BeDD.csproj` 里的，除非用 `-p:ProductVersion=` 指定。WiX 来自 NuGet；构建即表示接受它的 EULA
（`packaging/wix/Directory.Build.props` 里的 `AcceptEula`）。

```powershell
dotnet build packaging/wix/Bundle/BeDD.Bundle.wixproj -c Release -p:Platform=x64
# -> packaging\wix\Package\bin\x64\Release\bedd.msi、packaging\wix\Bundle\bin\x64\Release\bedd-setup.exe
```

`tests/installer/installer.test.mjs` 为当前用户安装、升级、卸载它们并逐步检查（见文件开头；CI 会运行它）。已经装了 BeDD 的机器上它拒绝运行。

**开发循环。** `bin\app\BeDD.exe dev` 是刚构建好的模组的试验场：它为开发用的槽位克隆基础实例（`instance clone`，默认槽位 9，可用 `--slot` 改），
把 `bin\BeDD` 装进克隆（`mod install`，或用 `--from <dir>` 指定），在后台启动游戏（`launch`，标签默认 `dev`，可用 `--label` 改），然后保持运行。
期间它把每次前台窗口和光标锁定矩形的变化记录到 `tools\out\desktop-watch.log`（或 `--watch-log <file>`）：
以 `MINECRAFT HAS THE FOREGROUND` 结尾的行表示有游戏抢了焦点（dev 同时会打印一条警告），锁定矩形要一直是 `full`。
这份日志就是游戏没有抢用户鼠标键盘的证据。

```powershell
bin\app\BeDD.exe dev                 # Ctrl+C 关掉游戏并退出
bin\app\BeDD.exe dev --slot 3 --keep # dev 退出时让游戏继续运行
bin\app\BeDD.exe dev --watch-only    # 只监视桌面（游戏由别的方式启动）
```

Ctrl+C 或创建 `tools\out\dev.stop` 会关掉游戏（除非 `--keep`）并结束 dev；`tools\out\dev.restart` 会关掉游戏、重新安装重新构建的模组并再次启动。
游戏退出时 dev 也会结束。它只会关闭从自己的克隆启动的游戏，同一个槽位同时只能有一个 dev 循环
（锁是 `%LOCALAPPDATA%\BeDD\locks\dev-<slot>.lock`，dev 无论怎样退出都会释放）。`BeDD.exe` 是控制台程序，shell 会一直等到 dev 结束。

## 文档与展示素材

`docs/` 是文档站点（<https://docs.levimc.org/BeDD/>，由 `.github/workflows/docs.yml` 用 levimc-kit 构建；`cd docs; npm ci; npm run build`）。
`docs/public/showcase/` 存放站点的展示内容和 README 里的图片。

`tools/showcase/record.mjs [--keep-open] [--dry-run]`（Node 24，无依赖）重新录制桌面视频：用 `BeDD.exe instance clone` 做三个克隆、
安装模组、恢复 `tools/showcase/fixtures/Showcase.zip` 存档，用 `BeDD.exe launch` 启动游戏并通过 WebSocket 操控（优先 bedboo 操作，
菜单用 `ui`，只有没有语义操作时才用原始输入），同时用 ffmpeg 把管理窗口录到 `tools/showcase/out/take-<n>-manager.mkv`，把脚本自己的日志录到
`take-<n>-terminal.mkv`。不会有窗口跳到你面前，也不会抢焦点。带 `--keep-open` 时，创建 `tools/showcase/out/take` 开始下一次录制，
`tools/showcase/out/stop` 结束。`node tools/showcase/encode.mjs` 把一次录制拼成左边管理窗口、右边终端的画面，输出 MP4、WebP 和封面图。

## 源码结构

| 文件 | |
|---|---|
| `src/BeDD.h` | 共享状态，以及下面各部分的声明 |
| `src/Mod.cpp` | 入口、配置、虚拟按键/光标状态 |
| `src/Window.cpp` | 窗口查找、子类化、合成事件分发、焦点屏蔽、让窗口留在屏幕外 |
| `src/ApiHooks.cpp` | user32 钩子（光标、按键状态、前台、激活、原始输入） |
| `src/GameInputHooks.cpp` | GameInput v2 钩子（虚拟鼠标、键盘读数） |
| `src/Audio.cpp` | FMOD 钩子：静音游戏的主通道组（`mute`） |
| `src/NetHooks.cpp` | 只用本机网络（`loopbackOnly`：没有防火墙提示，不查询本机以外的服务器）；服务端目标也有 |
| `src/ServerTimeScale.cpp` | 仅服务端目标：`server`、`timescale`、`command` 操作，配置 `timeScale`，向玩家发 LevelEvent 3007 |
| `src/Pipe.cpp` | 各操作：`handle()` 分派、按连接的归属与清理 |
| `src/WsServer.cpp`、`src/Discovery.cpp` | WebSocket 服务器（127.0.0.1、令牌、`/v1`）和 `run\<pid>.json` |
| `src/Screenshot.cpp` | `screenshot` 操作（PrintWindow + WIC） |
| `src/UiTree.cpp`、`src/UiJson.cpp`、`src/UiActions.cpp` | `ui` 操作：JSON UI 控件树、引用、click/setText/scroll |
| `src/UiOre.cpp` | `oreui` 操作：通过游戏的 V8 读取 Ore UI 的 DOM |
| `src/Manager.cpp` | `manager` 和 `close` 操作：管理窗口挂接期间保持隐藏 |
| `src/Trace.cpp` | trace 模式的输入日志 |
| `src/MoveInput.cpp` | `input` 操作：在客户端的 `ClientInputUpdateSystem` 里覆盖移动，带死人开关 TTL（`gameHooks`） |
| `src/Send.cpp` | `send` 操作（`allowSend`）：允许列表内的服务端方向数据包，在客户端线程上由本地玩家发送 |
| `src/GameState.cpp`、`src/Look.cpp` | `world` 快照和绝对 `look`（`gameHooks`） |
| `src/GameActions.cpp` | `slot`、`action` 和按刻计算的 `button` 操作（`gameHooks`） |
| `src/Stack.cpp` | `stack` 操作（`allowStack`）：由本地客户端发送的物品堆请求 |
| `src/TapSink.h` | 所有 tap 发布者共用的帧类型和 sink 链（保持逐字节一致） |
| `src/Tap.cpp`、`src/Tap.h` | tap：帧格式、标记、写线程、`tap` 操作及其 WebSocket 订阅方 |
| `src/NetTap.cpp` | 远程服务器连接上的 tap 抓取，以及 blob 缓存开关 |
| `src/TapLoopback.cpp` | 单人世界和局域网主机世界里的 tap 抓取（重新序列化的包） |
| `src/TapJournal.cpp` | tap 的加入日志（给晚到读取方的压缩回放） |
| `src/MemoryOperators.cpp` | 让 `new`/`delete` 走 LeviLamina 的分配器（LeviLamina 要求） |
| `app/` | `BeDD.exe`（C# NativeAOT）：`Cli/` 命令，`Core/` 实例、启动、服务器、发现文件、WebSocket 客户端，`Gui/` 管理窗口；`BeDD.Tests/` |
| `tests/ws/` | 针对运行中游戏的协议测试（`node --test tests/ws/`） |
| `packaging/wix/` | 安装程序（WiX Toolset 7）：`Package/` 按用户安装的 MSI，`Bundle/` 套在外面的 setup.exe |
| `tests/installer/` | 安装程序的安装、升级、卸载测试（`node --test tests/installer/installer.test.mjs`） |
| `spec/bedd.asyncapi.yaml` | WebSocket 接口（AsyncAPI 3.0） |

挂钩游戏或 LeviLamina 的符号时，先查 `%LOCALAPPDATA%\.xmake\packages\l\levilamina\<version>\<hash>\include` 下的 SDK 头文件。
只调用 `MCAPI`、`MCFOLD` 和 `MCTAPI` 函数。

## Pull request

1. Fork 仓库，从 `main` 建分支。
2. 每个 pull request 只做一件事。说明它做了什么、你怎么测试的。
3. 遵循现有风格：C++20、仓库里的 `.clang-format` 和 `.editorconfig`，代码和注释用英文。
4. 在游戏里测试：`bin\app\BeDD.exe dev`，然后通过 `BeDD.exe` 驱动游戏，检查输入有效、游戏从不抢前台（`tools/out/desktop-watch.log`），
   真实输入也到不了游戏。
5. CI 通过后才能合并。

提交信息用祈使语气（"Add wheel tilt"，而不是 "Added …"）。对用户可见的改动，在
[CHANGELOG.md](https://github.com/LiteLDev/BeDD/blob/main/CHANGELOG.md) 的 *Unreleased* 下记一笔。

另见 [CONTRIBUTING.md](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md)。参与贡献即表示你同意你的贡献以本项目的许可证
[LGPL-3.0-or-later](https://github.com/LiteLDev/BeDD/blob/main/COPYING.LESSER) 授权。
