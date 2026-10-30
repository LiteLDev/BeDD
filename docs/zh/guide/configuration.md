# 配置

模块读取 `mods\BeDD\config\config.json`，一般由测试框架写入（`Install-BeDDMod` 会写）。
没有这个文件时模块是被动的：后台模式关闭，管道照样能用。

```json
{ "background": true, "placement": "back", "clientWidth": 1600, "clientHeight": 900, "pipeName": "BeDD" }
```

`Install-BeDDMod` 和 `Start-BeDDGame.ps1` 还会把实例的 LeviLamina 切到内置崩溃记录器（`mods\LeviLamina\config\Config.json`
里的 `modules.crashLogger.builtin`）：默认的 `CrashLogger.exe` 是控制台程序，每次启动游戏都会在桌面上闪一下黑窗口。

## 配置项

| 键 | |
|---|---|
| `background` | 让游戏待在后台，不受你真鼠标键盘的影响 |
| `placement` | `back`（放到你的窗口后面）、`offscreen`（移出屏幕）或 `none` |
| `clientWidth`、`clientHeight` | 游戏窗口大小（0 表示不改），每次运行点击位置都一样 |
| `pipeName` | 多开时每个实例一个 |
| `loopbackOnly` | 默认 `true`：游戏只在本机通信，新实例不会弹 Windows 防火墙提示，也不会去查询本机以外的游戏服务器（否则它们可能以"局域网世界"出现在测试存档前面）。测试框架自己启动的专用服务器每个服务器目录还是会问一次，点**取消**即可 |
| `manager` | 默认 `true`：后台模式下，有管理窗口时游戏显示在管理窗口里，自己的窗口留在屏幕外、不进任务栏 |
| `mute` | 默认 `true`：后台模式下游戏不出声（你接管游戏时恢复声音） |
| `trace` | 把到达游戏的每个输入写进 `logs\latest.log`，报 bug 时用 |
| `pipeInstances` | 默认 `4`：可同时使用游戏管道的脚本数（某个脚本断开时，它按住的键、鼠标按钮和 `input` 移动覆盖都会松开）；`1` 表示一次一个 |

## 给 bedboo 用的配置项

这些配置项打开 [bedboo](https://github.com/LiteLDev/bedboo) 的 `bedd` 传输要用的操作。默认全部关闭；除非测试框架需要，
否则保持关闭（见[安全](/zh/reference/protocol#安全)）。

| 键 | |
|---|---|
| `gameHooks` | 默认 `false`：挂钩游戏自身的系统，用于 `world` 与 `look`（`Get-BeDDWorld`、`Set-BeDDLook`）、`input`（`Set-BeDDInput`：行走、跳跃、潜行、疾跑，与按键绑定无关）、`slot`、`action` 与 `button`（`Set-BeDDSlot`、`Invoke-BeDDAction`、`Set-BeDDButton`：快捷栏、丢弃/背包/视角/暂停、按游戏刻或直到目标方块破坏为止按住攻击/使用/选取） |
| `tap` | 默认 `false`：提供 `\\.\pipe\<pipeName>-tap`，一个二进制流，包含客户端收发的游戏数据包（[Tap 协议](/zh/reference/tap)），并带加入日志，读取方可在会话中途接入。它包含登录玩家的整个会话，所以只有你的用户能打开。打开时客户端会请服务器不使用它的 blob 缓存，每个区块都完整发送 |
| `tapLoopback` | 默认 `true`，只在 `tap` 打开时生效：单人世界和本游戏开的局域网世界也接到 tap 管道上（数据包重新序列化；局域网访客的流量不包括在内） |
| `tapJournalMiB` | 默认 `128`：tap 加入日志的大小上限；超过后中途接入不完整（`complete:false`） |
| `allowSend` | 默认 `false`：允许管道操作 `send`（`Send-BeDDPacket`），bedboo 等脚本借此让客户端发送它序列化好的聊天和命令（你电脑上的任何程序都能以你的玩家身份这样做） |
| `sendAllow` | `send` 可发送的数据包 id，默认 `[9, 77]`（text、command_request）；login、inventory_transaction、player_auth_input 和 item_stack_request 始终被拒绝 |
| `allowStack` | 默认 `false`：`stack` 操作（`Send-BeDDStack`）让管道客户端以登录玩家的身份发出物品堆请求（背包移动、合成、箱子存取），bedboo 的 bedd 模式用它。只在需要的测试里打开 |

## 环境变量

| 变量 | 谁读取 | |
|---|---|---|
| `BEDD_PIPE` | `BeDD.psm1` | `Connect-BeDD` 不带 `-PipeName` 时用的管道（默认 `BeDD`） |
| `BEDD_MANAGER` | `Start-BeDDGame.ps1`、框架垫片 | `0`：不用管理窗口 |
| `BEDD_LABEL` | 模块 | 游戏在管理窗口里的名字（`Start-BeDDGame.ps1 -Label` 会设置它） |
| `BEDD_RETURN_FOCUS` | 模块 | 一个窗口句柄：游戏启动时若仍激活了自己，前台还给这个窗口（`Start-BeDDGame.ps1` 和垫片的 `Start-Game` 会设置它） |
| `BEDD_DIR` | 你的测试框架 | Be.D.D. 解压的位置（[安装](/zh/guide/installation#让测试框架自己下载)） |
| `BEDD_INSTANCE`、`BEDD_VERSIONS_DIR`、`BEDD_LAUNCHER_DATA`、`BEDD_LEVILAUNCHER`、`BEDD_LIP` | `BeDDSetup.psm1` | 游戏实例和工具在哪里（[在新电脑上准备](/zh/guide/new-pc#用哪个实例)） |
