# Be.D.D.

<img src="docs/public/logo.png" alt="" width="128" align="right">

[English](README.md) | 简体中文

[![Release](https://img.shields.io/github/v/release/LiteLDev/BeDD)](https://github.com/LiteLDev/BeDD/releases)
[![License](https://img.shields.io/github/license/LiteLDev/BeDD)](COPYING.LESSER)
[![Docs](https://img.shields.io/badge/%E6%96%87%E6%A1%A3-docs.levimc.org%2FBeDD-blue)](https://docs.levimc.org/BeDD/zh/)

Be.D.D.（Bedrock Deep Dive）得名自《绝区零》的 H.D.D.：绳匠在远处引导代理人穿行空洞，你的脚本在后台引导 Minecraft。

让 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组的端到端测试**在后台跑**。
脚本在 Minecraft 基岩版里按键、打字、点击，你的鼠标、键盘和窗口照常归你用，还能同时开好几个游戏一起测。

Be.D.D. 是一套测试工具，不是给玩家装的模组：测试框架在每次运行时放进游戏实例的客户端 LeviLamina 模块、驱动它的 PowerShell 客户端，以及准备、克隆、启动游戏的脚本。**文档：<https://docs.levimc.org/BeDD/zh/>**

<p align="center"><img src="docs/public/showcase/desktop.webp" alt="三个 Minecraft 窗口跟着测试脚本动，前面的终端一直保持焦点" width="100%"></p>

> [!WARNING]
> 这是**测试工具**。`mods\` 目录里有 Be.D.D. 的游戏不理会你真实的鼠标和键盘，而且只显示在管理窗口里。
> 别把它留在平时玩的游戏里。

## 功能

- **测试时照常用电脑。** 游戏不抢前台、不锁鼠标、不吃你的按键，你点哪儿都不会点进游戏里。
- **多开一起测。** 每个游戏实例一条自己的管道；克隆一个实例只要几秒钟。
- **一个窗口看全部。** 电脑上所有测试的游戏都实时显示在同一个管理窗口里，不会在桌面上弹来弹去；关掉它就关掉全部游戏。
- **跟真人玩一样。** 按键、聊天、键位绑定、菜单、悬停提示、点击、转视角都和真鼠标键盘一样生效。
- **结果确定。** 每个调用都等游戏处理完输入才返回，不用靠猜的 `sleep`。
- **窗口被挡住也能截图。** 管理窗口可以躲在你其他窗口后面。
- **直接接进现有测试框架。** 一个文件替换掉 LeviLauncher 测试框架里常见的 `Click`、`Press-Key`、`Focus-Game` 等函数，测试场景一行不用改。

<p align="center"><img src="docs/public/showcase/how-it-feels.svg" alt="你的输入只到你自己的应用；脚本通过各自的管道驱动三个后台游戏" width="100%"></p>

## 安装

从 [Releases](https://github.com/LiteLDev/BeDD/releases) 下载 `bedd-client-windows-x64.zip`，解压到任意位置：里面是模块
（`BeDD.dll`）和脚本。测试框架会在每次运行开始时把模块放进游戏实例的 `mods\` 目录、结束后删掉
（[见下文](#接入测试框架)）；想手动试一下，就解压到该实例的 `mods\` 目录。Be.D.D. 不是 lip 包，也不在 LeviMC 模组里上架。

需要 Minecraft 基岩版 **1.26.51**（GDK 版）和 LeviLamina **26.51.x**（客户端），装在用 LeviLauncher 安装的游戏实例里。
测试时用 `Start-BeDDGame.ps1` 启动这个实例，不走启动器（[见下文](#多开)）。只有测试机上的游戏需要装，服务器和玩家都不用。

### 新电脑上

除了游戏本身，什么都不用提前手动准备。`Initialize-BeDD.ps1`（`Start-BeDDGame.ps1` 和 `clone-instance.ps1` 也会调用它）
会找到游戏实例，并把电脑上缺的东西补齐，每一步只做一次：

| 缺什么 | 怎么处理 |
|---|---|
| LeviLauncher | 用已安装的；没有就把最新版下载到 `%LOCALAPPDATA%\BeDD\tools` |
| 游戏本身 | 打开 LeviLauncher，脚本等你用拥有 Minecraft 的微软账号登录并装好 1.26.51（这一步脚本做不了；设了 `CI` 环境变量时直接报错并给出这些说明） |
| LeviLamina | 用 [lip](https://github.com/LiteLDev/lip) 装进实例；lip 和它需要的 .NET 运行时会自动下载（按用户安装，不需要管理员） |
| 游戏 exe 里 LeviLauncher 的加载器 | 通过 LeviLauncher 启动一次该实例让它加上，然后关掉游戏。没有加载器的话，游戏会用你平时的 Minecraft 数据，而不是实例自己的 |
| 克隆（`-Slot 2` 及以上） | 从基础实例克隆；基础实例之后有变化时会同步更新 |

```powershell
$i = & .\Initialize-BeDD.ps1 -Slot 2    # versions\<基础实例>-hi2 的 Name、Dir、ModsDir、PipeName
```

BeDD 从 `%APPDATA%\levilauncher.exe` 找 LeviLauncher 的数据，遵循它的"数据目录"设置（`base_root`），
并在其中挑 1.26.51 的实例（优先装了 LeviLamina 的）。要指向别处，设置 `BEDD_INSTANCE`（实例名）、`BEDD_VERSIONS_DIR`、
`BEDD_LAUNCHER_DATA`、`BEDD_LEVILAUNCHER`（LeviLauncher.exe）或 `BEDD_LIP`（lip.exe）。

测试框架也可以用同样的方式自己下载 BeDD，不必要求某个目录下事先有一份检出：

```powershell
$bedd = if ($env:BEDD_DIR) { $env:BEDD_DIR } else { Join-Path $env:LOCALAPPDATA 'BeDD\client' }
if (-not (Test-Path "$bedd\harness-shim.ps1")) {
    $zip = Join-Path $env:TEMP 'bedd.zip'
    Invoke-WebRequest https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-client-windows-x64.zip -OutFile $zip
    Expand-Archive $zip (Split-Path $bedd) -Force; Rename-Item (Join-Path (Split-Path $bedd) 'BeDD') $bedd
}
```

## 使用方法

PowerShell 模块就放在模组旁边：

```powershell
Import-Module .\mods\BeDD\BeDD.psm1
Connect-BeDD                          # 等游戏启动好
Send-BeDDKey E                             # 打开背包
Send-BeDDMouse click -RX 0.5 -RY 0.474     # 按窗口比例位置点击
Send-BeDDKey T; Send-BeDDText '/give @s apple 3'; Send-BeDDKey ENTER
Save-BeDDScreenshot C:\temp\shot.png       # 窗口被挡住也能截
```

| 命令 | 作用 |
|---|---|
| `Connect-BeDD [-PipeName p]` | 等游戏就绪，并选定之后的调用发给哪个游戏 |
| `Send-BeDDKey E` | 按一下键；`-Action down`/`up` 按住或松开，`-Vk 0x10` 指定任意虚拟键码 |
| `Send-BeDDText '...'` | 输入文字（不产生按键，所以 `t` 不会打开聊天栏）；`-DelayMs` 设定每个字的间隔 |
| `Send-BeDDMouse click -RX 0.5 -RY 0.5` | `move`、`down`、`up`、`click`、`wheel`，位置用 `-X -Y` 像素或 `-RX -RY` 比例；`-Button right` 右键 |
| `Send-BeDDRaw move -Dx 200` | 在世界里转视角；`click -Button right` 使用物品 |
| `Clear-BeDDInput` | 松开所有还按着的键和鼠标按钮 |
| `Save-BeDDScreenshot path.png` / `Get-BeDDBitmap` | 游戏画面，被挡住也照样截 |
| `Set-BeDDWindow 1600 900` | 调整游戏窗口大小，不会把它提到前面 |
| `Get-BeDDState` | 游戏是否在线、收到了多少输入、是否抢过前台 |
| `Get-BeDDWorld` | 是否在世界中、当前界面、鼠标是否锁定、是否打开菜单、玩家位置与朝向（`gameHooks`） |
| `Set-BeDDLook 90 -30` | 把玩家转到指定的偏航角和俯仰角（度，`gameHooks`） |
| `Set-BeDDInput -Z 1 -Sprint -TtlMs 2000` | 通过游戏自己的输入系统让玩家行走、跳跃、潜行或疾跑一段时间，与按键绑定无关（`gameHooks`） |
| `Set-BeDDSlot 3` / `Invoke-BeDDAction inventory` | 选择快捷栏格子；丢弃、全部丢弃、打开背包、切换视角、暂停（`gameHooks`） |
| `Set-BeDDButton attack -Action down -ReleaseOnBlockChange` | 按游戏刻或直到目标方块破坏为止按住攻击/使用/选取，不阻塞管道（`gameHooks`） |
| `Send-BeDDPacket -Base64 ...` | 让玩家发送脚本序列化好的聊天或命令数据包（`allowSend`） |
| `Send-BeDDStack -Buffer ...` / `-View` | 让玩家发送脚本序列化好的物品堆请求；`-View` 显示客户端看到的背包（`allowStack`） |

按键名：字母、数字、`ESC`、`ENTER`、`BACK`、`TAB`、`SPACE`、`SHIFT`、`CTRL`、`ALT`、方向键（`LEFT` 等）、
`HOME`、`END`、`PGUP`、`PGDN`、`INSERT`、`DELETE`、`F1`..`F24`、`SLASH`、`GRAVE`、`MINUS`、`EQUALS`。

### 多开

```powershell
pwsh -NoProfile -File clone-instance.ps1 -Slot 2                  # versions\1.26.51.01-hi2，约 90 MB，几秒钟
& .\Start-BeDDGame.ps1 -Instance 1.26.51.01-hi2                # 启动时不把窗口提到前面
Connect-BeDD -PipeName BeDD-1.26.51.01-hi2   # 该实例配置里的 pipeName
```

克隆不存在时，`Start-BeDDGame.ps1 -Slot 2`（或 `-Instance <基础实例>-hi2`）会自己先克隆出来。`clone-instance.ps1` 只接受 1 到 32 号槽位；更大的槽位用 `BeDDSetup.psm1` 的 `New-BeDDClone -Source <基础实例> -Slot 41`。

克隆出来的实例有自己的 `mods\`、日志、存档和设置。几条规矩，免得并行的测试互相打架：

- 每个实例用自己的锁文件；按**实例目录**找自己的游戏进程，别只看进程名 `Minecraft.Windows`（不然会关掉别人的游戏）。
- 一定以 `Minecraft.Windows.exe` 这个文件名启动（`Start-BeDDGame.ps1` 就是这样做的）：换个名字，游戏会悄悄改用你平时玩的
  Minecraft 存档目录，而不是这个实例的。
- 测试存档用 `Set-WorldSinglePlayer <存档目录>`（框架垫片里提供）设成单人：同一台电脑上的游戏共用一个联机登录，
  多个多人存档同时打开会报"发生错误"（代码 "Blaze"）。
- 需要在线加入服务器的测试，在 `Start-Game` 之前设 `$script:ExclusiveSignaling = $true`，等其他游戏关掉后独占整台电脑。

### 管理窗口

本次登录后第一次运行 `Start-BeDDGame.ps1` 时会打开**管理窗口**，之后的运行（不管来自哪个终端、测试框架或 agent）都复用它。
每个游戏都以实时画面显示在里面，也只显示在里面：游戏自己的窗口留在屏幕外，任务栏上没有按钮，Alt+Tab 里也没有，
多个测试同时启动、关闭游戏时桌面上不会有窗口弹出又消失。它也从不抢焦点。

- 每个游戏标注启动它的脚本所在的项目（和 worktree）、实例名、pid，以及是否正在收到输入。`Start-BeDDGame.ps1 -Label` 可以自己起名。
- **关掉管理窗口会关掉里面所有的游戏**（会先确认）。管理窗口被强制结束时游戏也会一起退出，不会在后台留下游戏。
- 双击某个游戏只看它一个。右键可以打开它的 `latest.log` 或实例目录，或者关掉它。
- 脚本里：`BeDDManager.ps1 -List` 列出游戏，`-Game <pid> -Action close` 关掉一个游戏，
  `-Shutdown` 不经确认关掉管理窗口和所有游戏。

需要 PowerShell 7（`pwsh`）。游戏从不脱离管理窗口运行：找不到管理窗口的游戏会自己启动一个（配置里的 `managerScript`）；
90 秒内没有管理窗口连上、或者彻底失去管理窗口的游戏会自行关闭（原因写在它的 `latest.log` 里）。

### 接入测试框架

`harness-shim.ps1` 原地替换 LeviLauncher 测试框架的常用函数（`Focus-Game`、`Assert-GameForeground`、
`Set-GameWindowSize`、`Move-Mouse`、`Click`、`Scroll`、`Press-Key`、`Type-Text`、`Get-ClientBitmap`，参数不变），
并包装 `Start-Game`/`Stop-Game`：

```powershell
. "$PSScriptRoot/lib.ps1"
. "$beddDir\harness-shim.ps1"
Register-BeDDNative ([MyE2E.Native])   # 可选，见下文
# 部署完你的模组、把其他模组挪开之后：
Install-BeDDMod -ModsDir $script:ModsDir -PipeName 'BeDD-<实例名>'
# ……在 finally 里、游戏关掉之后：
Uninstall-BeDDMod -ModsDir $script:ModsDir
```

游戏用 `Start-BeDDGame.ps1` 启动，不要走启动器；"把其他模组挪开"那一步要跳过 `BeDD`：它每次运行都重新装、
结束就删（留下一份的话，正常启动的游戏也会进管理窗口）。

如果测试场景直接调用框架自己的输入类（比如 `[MyE2E.Native]::Key(0x10, $false)`），给这个类加上下面几个字段，
并在每个方法开头加一行判断，`Register-BeDDNative` 会把它们填好：

```csharp
public static Action<ushort, bool> KeyRedirect;          // (vk, up)
public static Action<int, bool>    MouseButtonRedirect;  // (0 左 / 1 右 / 2 中, up)
public static Action<int>          WheelRedirect;        // (delta，每格 120)
public static Action<char>         UnicodeRedirect;
public static Action<int, int>     MoveRelativeRedirect; // 视角
public static void Key(ushort vk, bool up) { if (KeyRedirect != null) { KeyRedirect(vk, up); return; } /* 原来的 SendInput */ }
```

完整的测试框架可以参考 [BeetrootSkin](https://github.com/LiteLDev/BeetrootSkin/tree/main/tools/e2e) 和
[AlleviatedBackpacks](https://github.com/LiteLDev/AlleviatedBackpacks/tree/main/tools/e2e)。

## 设置

`mods\BeDD\config\config.json`，一般由测试框架写入（`Install-BeDDMod` 会写）。
没有这个文件时游戏照样无头运行，只是用默认值、没有 `managerScript`（所以只能进已经开着的管理窗口）。`Install-BeDDMod` 和 `Start-BeDDGame.ps1`
还会把实例的 LeviLamina 切到内置崩溃记录器（`mods\LeviLamina\config\Config.json` 里的
`modules.crashLogger.builtin`）：默认的 `CrashLogger.exe` 是控制台程序，每次启动游戏都会在桌面上闪一下黑窗口。

```json
{ "clientWidth": 1600, "clientHeight": 900, "pipeName": "BeDD", "managerScript": "C:\\BeDD\\BeDDManager.ps1" }
```

| 键 | |
|---|---|
| `clientWidth`、`clientHeight` | 游戏窗口大小（0 表示不改），每次运行点击位置都一样 |
| `pipeName` | 多开时每个实例一个 |
| `loopbackOnly` | 默认 `true`：游戏只在本机通信，新实例不会弹 Windows 防火墙提示，也不会去查询本机以外的游戏服务器（否则它们可能以"局域网世界"出现在测试存档前面）。测试框架自己启动的专用服务器每个服务器目录还是会问一次，点**取消**即可 |
| `managerScript` | `BeDDManager.ps1` 的路径（`Install-BeDDMod` 会写）：没有管理窗口时游戏用它启动一个 |
| `mute` | 默认 `true`：游戏不出声 |
| `trace` | 把到达游戏的每个输入写进 `logs\latest.log`，报 bug 时用 |
| `pipeInstances` | 默认 `4`：可同时使用游戏管道的脚本数（某个脚本断开时，它按住的键、鼠标按钮和 `input` 移动覆盖都会松开）；`1` 表示一次一个 |
| `gameHooks` | 默认 `false`：挂钩游戏自身的系统，供 bedboo 用的操作使用：`world` 与 `look`（`Get-BeDDWorld`、`Set-BeDDLook`）、`input`（`Set-BeDDInput`：行走、跳跃、潜行、疾跑，与按键绑定无关）、`slot`、`action` 与 `button`（`Set-BeDDSlot`、`Invoke-BeDDAction`、`Set-BeDDButton`：快捷栏、丢弃/背包/视角/暂停、按游戏刻或直到目标方块破坏为止按住攻击/使用/选取）；见 CONTRIBUTING.md |
| `tap` | 默认 `false`：提供 `\\.\pipe\<pipeName>-tap`，一个二进制流，包含客户端收发的游戏数据包，供 [bedboo](https://github.com/LiteLDev/bedboo) 的 `bedd` 传输使用（见 CONTRIBUTING.md 的 Tap protocol），并带加入日志，读取方可在会话中途接入。它包含登录玩家的整个会话，所以只有你的用户能打开。打开时客户端会请服务器不使用它的 blob 缓存，每个区块都完整发送 |
| `tapLoopback` | 默认 `true`，只在 `tap` 打开时生效：单人世界和本游戏开的局域网世界也接到 tap 管道上（数据包重新序列化；局域网访客的流量不包括在内） |
| `tapJournalMiB` | 默认 `128`：tap 加入日志的大小上限；超过后中途接入不完整（`complete:false`） |
| `allowSend` | 默认 `false`：允许管道操作 `send`（`Send-BeDDPacket`），bedboo 等脚本借此让客户端发送它序列化好的聊天和命令（你电脑上的任何程序都能以你的玩家身份这样做） |
| `sendAllow` | `send` 可发送的数据包 id，默认 `[9, 77]`（text、command_request）；login、inventory_transaction、player_auth_input 和 item_stack_request 始终被拒绝 |
| `allowStack` | 默认 `false`：`stack` 操作（`Send-BeDDStack`）让管道客户端以登录玩家的身份发出物品堆请求（背包移动、合成、箱子存取），bedboo 的 bedd 模式用它。只在需要的测试里打开 |

## 更多

完整文档（含更多视频和截图）：<https://docs.levimc.org/BeDD/zh/>

想自己构建、了解管道协议或内部原理？见 [CONTRIBUTING.md](CONTRIBUTING.md#development)。

## 许可证

[LGPL-3.0-or-later](COPYING.LESSER) © LeviMC。GameInput 接口布局参考微软公开的 `GameInput.h`（v2），不包含任何微软代码。
