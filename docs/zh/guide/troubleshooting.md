# 常见问题

## 游戏不理我的鼠标和键盘

后台模式开着：模块在实例的 `mods\` 目录里，配置打开了后台模式，常常是测试框架留下的副本。在游戏窗口里按
**Ctrl+Shift+F12** 关掉后台模式，然后删掉 `mods\BeDD`。测试框架会在 `finally` 里调用 `Uninstall-BeDDMod`，
并在"把其他模组挪开"那一步跳过 `BeDD`，以免出现这种情况。

## 多开时报"发生错误"（代码 "Blaze"）

同一台电脑上的游戏共用一个联机登录，多个多人存档同时打开会失败。用 `Set-WorldSinglePlayer <存档目录>` 把测试存档设成单人。
必须在线加入服务器的测试，在 `Start-Game` 之前设 `$script:ExclusiveSignaling = $true`，等其他游戏关掉后独占整台电脑。
见[并行测试的规矩](/zh/guide/several-games#并行测试的规矩)。

## 游戏用了我平时的 Minecraft 存档和设置

只有以 `Minecraft.Windows.exe` 启动、且 exe 里有 LeviLauncher 的加载器时，游戏才用实例自己的数据。用
`Start-BeDDGame.ps1` 启动就两者都满足：[准备步骤](/zh/guide/new-pc)会通过 LeviLauncher 启动一次实例来加上加载器。

## 每个新实例都弹 Windows 防火墙提示

`loopbackOnly`（默认打开）时游戏只在本机通信，不会弹提示。测试框架自己启动的专用服务器每个服务器目录还是会问一次，点**取消**即可。

## 启动游戏时闪一下控制台窗口

那是 LeviLamina 的 `CrashLogger.exe`。`Install-BeDDMod` 和 `Start-BeDDGame.ps1` 会把实例切到内置崩溃记录器；
其他方式准备的实例，在 `mods\LeviLamina\config\Config.json` 里设 `modules.crashLogger.builtin`。

## 我关掉了别人的游戏

按**实例目录**找自己的游戏，别只看进程名 `Minecraft.Windows`；每个实例用自己的锁文件。

## 关掉管理窗口，所有游戏都关了

它就是这样设计的（会先确认）：电脑上所有测试的游戏都在里面。想让某个游戏不进管理窗口，用
`Start-BeDDGame.ps1 -NoManager`（或 `BEDD_MANAGER=0`）启动，并用 `Install-BeDDMod -NoManager` 安装模块。

## `Set-BeDDLook` 超时，或暂停后游戏操作没反应

暂停的单人世界不走游戏刻，而 `look`、`slot`、`action` 和 `button` 都在客户端游戏刻里执行。在单人世界
`Invoke-BeDDAction pause` 之后，用 `Send-BeDDKey ESC` 恢复。

## 某个操作返回 `code:"disabled"`

这个操作需要打开配置项：`gameHooks`、`tap`、`allowSend` 或 `allowStack`（[配置](/zh/guide/configuration#给-bedboo-用的配置项)）。
当前版本没有的操作返回 `unknown op`。

## 报告 bug

在 [GitHub](https://github.com/LiteLDev/BeDD/issues/new/choose) 开 issue，附上：

- Be.D.D.、LeviLamina 和 Minecraft 的版本；
- 脚本发了什么（JSON 请求或 PowerShell 调用），游戏实际做了什么；
- `mods/BeDD/config/config.json`；
- `logs/latest.log` 的相关部分，最好打开 `"trace": true`（它显示哪些输入到达了游戏，以及是合成的还是真实的）。
