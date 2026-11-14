# 构建与贡献

## 构建

Windows x64，另需（`winget install -e --id <id>`；LLVM、Build Tools 需要管理员权限；装完重开终端）：

| 工具 | id |
|---|---|
| xmake | `Xmake-io.Xmake` |
| LLVM `clang-cl`（模组工具链） | `LLVM.LLVM` |
| Build Tools 2022/2026，C++（MSVC 14.4x+） | `Microsoft.VisualStudio.2022.BuildTools` / `Microsoft.VisualStudio.BuildTools` + `--override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"` |
| .NET 10 SDK | `Microsoft.DotNet.SDK.10` |
| Node.js 24+ | `OpenJS.NodeJS.LTS` |

| 构建 | 命令 | 输出 |
|---|---|---|
| 模组 | `$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"`<br>`xmake f -P . -y -m release -p windows --target_type=client`（`-m debug`）<br>`xmake -P . -y`<br>`git checkout -- xmake-requires.lock` | `bin\BeDD\` |
| 服务端模组 | `git checkout -- xmake-requires.lock`<br>`xmake f -P . -y -m release -p windows --target_type=server`<br>`xmake -P . -y`<br>`git checkout -- xmake-requires.lock` | `bin\BeDD-server\` → 服务器 `mods\BeDD-server\`（配置 `mods\BeDD-server\config\config.json`；[操作](/zh/reference/websocket#服务端目标)） |
| 应用 | `$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"`（AOT 需要 vswhere）<br>`dotnet publish app/BeDD/BeDD.csproj -c Release -o bin\app`<br>`dotnet test app/BeDD.slnx`（无需 vswhere） | `bin\app\BeDD.exe` |
| 安装程序 | `dotnet build packaging/wix/Bundle/BeDD.Bundle.wixproj -c Release -p:Platform=x64` | `packaging\wix\Package\bin\x64\Release\bedd.msi`、`packaging\wix\Bundle\bin\x64\Release\bedd-setup.exe` |
| 文档 | `cd docs; npm ci; npm run build` | <https://docs.levimc.org/BeDD/>（CI：`docs.yml`） |

- `-P .`：否则 xmake 可能构建上级检出（在 worktree 中时）。
- 版本取自 `git describe`：需要提交和标签（浅克隆时 `git fetch --tags`）。
- `xmake-requires.lock` 固定两个目标的依赖；xmake 会只按一个目标改写它：每次构建后恢复。新固定：配置两个目标，提交
  并集。
- 服务端模组：WebSocket、回环网络、`src/ServerTimeScale.cpp`（列表见 `xmake.lua`）。
- 安装程序（WiX 7 来自 NuGet，`AcceptEula` 在 `packaging/wix/Directory.Build.props`）：按用户安装的 MSI + `setup.exe`
  （英文/中文界面），来源 `bin\release\BeDD`（由 `.github/actions/stage` 从 `bin\app`、`bin\BeDD`、`bin\BeDD-server`、
  spec 生成）；版本取自 `app/BeDD/BeDD.csproj` 或 `-p:ProductVersion=`。测试：`tests/installer/installer.test.mjs`
  （当前用户；已安装 BeDD 的机器上拒绝运行；CI）。

## 开发循环

`bin\app\BeDD.exe dev`：克隆槽位 9（`--slot`），安装 `bin\BeDD`（`--from <dir>`），启动（标签 `dev`，`--label`），并把
前台和光标裁剪变化记录到 `tools\out\desktop-watch.log`（`--watch-log`）。以 `MINECRAFT HAS THE FOREGROUND` 结尾的行
（同时警告）：有游戏抢了焦点。裁剪必须保持 `full`。

Ctrl+C 或 `dev.stop`（日志目录中）：关闭游戏（除非 `--keep`）并退出；`dev.restart`：重新安装并重新启动；游戏退出时 dev
也退出。`--watch-only`：只监视。

只关闭自己克隆的游戏；锁 `%LOCALAPPDATA%\BeDD\locks\dev-<slot>.lock`；占住当前 shell。

## 文档与展示素材

`docs/public/showcase/`：画廊和 README 素材，由 `tools/showcase/record.mjs [--keep-open] [--dry-run]` 和 `encode.mjs`
生成（[详情](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md#docs-and-showcase)）。

## 源码结构

每个源文件及其内容：[CONTRIBUTING.md](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md#source-layout)。

代码规范（SDK 头文件，仅 `MCAPI`）、pull request、许可证：[CONTRIBUTING.md](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md)。
