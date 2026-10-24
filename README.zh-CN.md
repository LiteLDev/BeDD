# LeviHeadlessInput

<img src="docs/logo.png" alt="" width="128" align="right">

[English](README.md) | 简体中文

用脚本驱动 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 版 Minecraft 基岩版**客户端**，
**全程不碰你的鼠标、键盘和前台窗口**，还能**同时跑多个游戏实例**。

客户端 mod 的端到端测试通常对前台的游戏发送真实输入（`SendInput`）：测试期间没人能用电脑，一次也只能测一个游戏。
LeviHeadlessInput 是一个仅供测试使用的客户端 mod，配有 PowerShell 客户端。脚本通过命名管道把指令发给 mod，
由 mod 在游戏内部注入输入；游戏始终留在后台，并且不理会真实的输入设备。

> [!WARNING]
> 这是**测试工具**。加载后只要开着后台模式，游戏就会忽略你真实的鼠标和键盘。不要把它留在你平时玩的游戏里
> （在游戏窗口按 **Ctrl+Shift+F12** 可以关闭后台模式）。

适配 Minecraft **1.26.51.01**（GDK 版）与 LeviLamina **26.51.7**（客户端）。

## 原理

1. **键盘、文字和指针位置走游戏自己的窗口过程。** mod 子类化游戏窗口，把按键转成 `WM_KEYDOWN`/`WM_KEYUP`、
   文字转成 `WM_CHAR`、指针转成 `WM_MOUSEMOVE` 加一个虚拟光标（被 hook 的 `GetCursorPos` 返回它），
   在游戏线程上按顺序派发。游戏处理它们的方式和处理真实输入完全一样，聊天、按键映射、界面悬停都正常。
   只有游戏线程处理完之后才回复，所以脚本是确定性的。
2. **鼠标按键、滚轮和视角走虚拟 GameInput 鼠标。** 基岩版通过 GDK 的 **GameInput** API（`GameInputRedist.dll`，接口 v2）
   直接从设备读鼠标：游戏根本不看 `WM_LBUTTONDOWN` 这类消息。所以不加处理的话，即使游戏在后台，你的真实鼠标照样会点击、转视角。
   mod hook 了 `IGameInputReading::GetMouseState`（以及 `GetKeyCount`/`GetKeyState`），后台模式下游戏读到的是虚拟的按键、
   相对位移（视角）、滚轮和绝对位置。
3. **游戏不会抢前台，也不会失去焦点。** 失焦消息被吞掉，游戏不会暂停、不会松开按键。`Start-HeadlessGame.ps1` 以
   `SW_SHOWNOACTIVATE` 启动游戏；`ShowWindow`/`SetWindowPos`/`SetActiveWindow`/`BringWindowToTop`/`SetForegroundWindow`
   都被 hook，永远不会激活游戏窗口；窗口一直处于 Z 序最底层。
4. **真实输入到不了游戏。** 发往游戏客户区的真实输入消息会被吞掉；游戏用来读取或抓取设备的 user32 调用看到的都是**虚拟**状态：
   `ClipCursor` 不生效（光标永远不会被锁住），`SetCursorPos` 只移动虚拟光标，`GetCursorPos`、`GetAsyncKeyState`、
   `GetKeyState`、`GetKeyboardState`、`GetForegroundWindow` 等都返回虚拟值，真实的 raw input 会被丢弃。
5. **截图**使用 `PrintWindow(PW_RENDERFULLCONTENT)`，窗口被挡住时也能截。

## 安装

```powershell
lip install github.com/LiteLDev/LeviHeadlessInput     # 装到游戏实例的 mods\ 目录
```

也可以从 [Releases](https://github.com/LiteLDev/LeviHeadlessInput/releases) 下载
`leviheadlessinput-client-windows-x64.zip` 解压到 `mods\`。测试框架一般会在每次运行时自己部署它。

没有 `mods\LeviHeadlessInput\config\config.json` 时 mod 处于被动状态（后台模式关闭，管道照常工作）。测试框架会写入：

```json
{ "background": true, "placement": "back", "clientWidth": 1600, "clientHeight": 900, "trace": false, "pipeName": "LeviHeadlessInput" }
```

各字段的含义见[英文 README](README.md#install)。

## PowerShell 客户端

```powershell
Import-Module .\mods\LeviHeadlessInput\HeadlessInput.psm1
Connect-HeadlessInput                                   # 等待管道和游戏窗口就绪
Send-HeadlessKey E                                      # 按键；-Action down|up；-Vk 0x10
Send-HeadlessText '/give @s apple 3'                    # 输入文字（WM_CHAR，不产生按键）
Send-HeadlessMouse click -RX 0.5 -RY 0.474              # move|down|up|click|wheel；-X -Y 像素；-Button right
Send-HeadlessRaw move -Dx 200 -Dy 0                     # 转视角（相对位移）；down|up|click -Button right 使用物品
Save-HeadlessScreenshot C:\temp\shot.png
Get-HeadlessState                                       # 统计、被 hook 的 API 调用次数、是否在前台
Set-HeadlessBackground $false                           # 把游戏交还给真实设备
Set-HeadlessPipe LeviHeadlessInput-2                    # 切换到另一个实例
```

## 多开

既然不需要前台，就可以同时跑多个游戏实例，每个实例用自己的管道：

```powershell
pwsh -NoProfile -File clone-instance.ps1 -Slot 2                      # versions\1.26.51.01-hi2
& .\Start-HeadlessGame.ps1 -Instance 1.26.51.01-hi2                    # 不经过启动器，不激活窗口
Connect-HeadlessInput -PipeName LeviHeadlessInput-1.26.51.01-hi2
```

`clone-instance.ps1` 用硬链接（游戏文件）和目录联接（`data`、`Installers`）克隆 LeviLauncher 实例，只复制每个实例自己的状态
（`Minecraft Bedrock`：世界、设置、登录信息；`mods\LeviLamina`），一个克隆约 90 MB，几秒钟就能建好。测试框架需要注意：

- 每个实例（槽位）用自己的锁文件；
- 按**实例目录**识别游戏进程，不要只看进程名 `Minecraft.Windows`，否则会误操作、误关其他实例的游戏；
- 必须以 `Minecraft.Windows.exe` 这个文件名启动：LeviLauncher 的数据隔离按这个文件名生效，换了名字游戏会悄悄改用默认的
  `%APPDATA%\Minecraft Bedrock`（你平时玩的存档目录）；
- 游戏实例不会再弹 Windows 防火墙的"允许访问"：开启 `loopbackOnly`（默认）后，游戏只在本机回环地址上监听，对外连接不受影响。
  测试框架启动的专用服务器不在此列，每个新路径会问一次，点**取消**即可。

完整的测试框架示例见 [BeetrootSkin](https://github.com/LiteLDev/BeetrootSkin/tree/main/tools/e2e) 和
[AlleviatedBackpacks](https://github.com/LiteLDev/AlleviatedBackpacks/tree/main/tools/e2e)（`run.ps1 -Slot`、`Set-E2EInstance`）。

## 接入测试框架、协议

见[英文 README](README.md#use-it-from-a-harness)：`harness-shim.ps1` 可以原地替换常见的测试框架函数（`Click`、`Press-Key`、
`Focus-Game` 等，参数不变），并支持把测试框架 C# 类里的输入方法重定向到管道；管道协议是按行分隔的 JSON。

## 构建

需要 Windows x64、[xmake](https://xmake.io)、Visual Studio 生成工具（MSVC 14.4x 或更新）和 LLVM（`clang-cl`）：

```powershell
xmake f -a x64 -m release -p windows --target_type=client -y
xmake -y        # -> bin\LeviHeadlessInput\
```

## 许可证

[LGPL-3.0-or-later](COPYING.LESSER)。GameInput 接口布局参考微软公开的 `GameInput.h`（v2），不包含任何微软代码。
