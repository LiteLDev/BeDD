# 接入你的测试

Be.D.D. 没有需要加载的客户端库。任何语言写的测试框架都只做三件事：用 `BeDD.exe` 准备并启动游戏，通过它的
[WebSocket](/zh/guide/usage) 和它通信，最后关掉它。

```powershell
BeDD.exe instance clone --slot 2                         # 每次运行一次：克隆，保持最新
BeDD.exe mod install 1.26.60.05-2                        # BeDD 模组和它的配置
# 把你自己的模组复制进 (BeDD.exe instance path 1.26.60.05-2)\mods，恢复测试存档，然后：
BeDD.exe world singleplayer <存档目录>
BeDD.exe launch --slot 2 --label my-mod --detach --json  # pid、url、token
# ……测试连接 url?token=…………
BeDD.exe stop <pid>
```

- 每个命令在 stdout 上只输出结果；`--json` 输出一个 JSON 文档，`-q` 只输出标识符，`-v` 在 stderr 上输出更多细节。出错时输出
  `error: …` 和一条 `hint:`，提示一个可以直接运行的命令（`--json` 时为 `{"error":{code,message,hint}}`）。
- 退出码：`0` 成功，`1` 失败，`2` 用法错误，`4` 找不到，`5` 冲突（比如槽位正在使用），`10` 被拒绝（问你时你选了否）。
- 给每个并行的测试（worktree、CI 任务）分配自己的槽位。克隆的 `mods\` 归你用：每次运行把你的模组放进去。
  之后什么都不用删，因为克隆不是你平时玩的实例。
- `BeDD.exe dev` 把开发模组时常用的步骤打包在一起；见 `BeDD.exe dev --help`。

## 驱动游戏

优先使用不需要坐标和截图的操作（[操作模式](/zh/guide/operation-modes)）：

| 测试框架想要 | 用 |
|---|---|
| 移动、转视角、挖、使用、聊天、检查玩家状态 | [bedboo](https://github.com/LiteLDev/bedboo) 的 `transport: 'bedd'`，或 `input`、`look`、`button`、`world` 操作 |
| 点菜单按钮、填文本框 | `ui`（按 `role` 和 `name` 找节点，对它的 `ref` 操作） |
| 按键、输入文字 | `key`、`text` |
| 点击别的方式够不到的地方 | `mouse`（尽量用 `ui` 给的 `ref`，否则用 `rx`,`ry`） |
| 看屏幕 | `screenshot`，少用；大多数问题 `world` 和 `ui` 用文本就能回答 |

### 从 PowerShell 测试框架接入

用 PowerShell 写的测试框架可以保留测试场景，用 .NET 的 WebSocket 客户端几行代码替换掉输入函数：

```powershell
$game = (BeDD.exe launch --slot 2 --detach --json | ConvertFrom-Json)
$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ws.ConnectAsync([uri]"$($game.url)?token=$($game.token)", [Threading.CancellationToken]::None).Wait()
$script:BeDDId = 0
function Invoke-Op([hashtable]$Request) {
    $Request.id = ++$script:BeDDId
    $bytes = [Text.Encoding]::UTF8.GetBytes(($Request | ConvertTo-Json -Compress -Depth 10))
    $ws.SendAsync([ArraySegment[byte]]$bytes, 'Text', $true, [Threading.CancellationToken]::None).Wait()
    $buf = [byte[]]::new(1MB); $ms = [IO.MemoryStream]::new()
    do { $r = $ws.ReceiveAsync([ArraySegment[byte]]$buf, [Threading.CancellationToken]::None).Result
         $ms.Write($buf, 0, $r.Count) } until ($r.EndOfMessage)
    $reply = [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
    if (-not $reply.ok) { throw "BeDD $($Request.op): $($reply.error)" }
    $reply
}
function Press-Key($Key) { Invoke-Op @{ op = 'key'; key = $Key } | Out-Null }
function Click($RX, $RY) { Invoke-Op @{ op = 'mouse'; action = 'click'; rx = $RX; ry = $RY } | Out-Null }
```

`Focus-Game` 和 `Assert-GameForeground` 变成空操作：游戏从来不需要前台。

## 从 PowerShell 客户端迁移

Be.D.D. 1 附带一个 PowerShell 模块、一个测试框架垫片和一些脚本，通过命名管道和游戏通信。Be.D.D. 2 里这些都没有了：游戏讲
WebSocket，准备工作由 `BeDD.exe` 完成。每个旧命令都有替代：

| Be.D.D. 1 | Be.D.D. 2 |
|---|---|
| `Connect-BeDD [-PipeName p]` | 读 `run\<pid>.json` 或 `BeDD.exe ps --json`，打开 `url?token=…` |
| `Invoke-BeDD @{ op = … }` | 通过 WebSocket 发送同样的 JSON |
| `Send-BeDDKey` | `key` 操作 |
| `Send-BeDDText` | `text` 操作（或 bedboo 的 `bot.chat`） |
| `Send-BeDDMouse` | 目标是 JSON UI 控件时用 `ui` 操作的 `action:'click'`，否则用 `mouse` 操作 |
| `Send-BeDDRaw` | `look` 操作（bedboo），否则用 `raw` 操作 |
| `Save-BeDDScreenshot`、`Get-BeDDBitmap` | `screenshot` 操作（回复里是 base64 的 PNG/JPEG） |
| `Clear-BeDDInput` | `release` 操作 |
| `Set-BeDDWindow` | `window` 操作 |
| `Get-BeDDState` | `state` 操作 |
| `Get-BeDDWorld` | `world` 操作 |
| `Set-BeDDLook` | `look` 操作 |
| `Set-BeDDInput` | `input` 操作 |
| `Set-BeDDSlot` | `slot` 操作 |
| `Invoke-BeDDAction` | `action` 操作 |
| `Set-BeDDButton` | `button` 操作 |
| `Send-BeDDPacket` | `send` 操作 |
| `Send-BeDDStack` | `stack` 操作 |
| `Get-BeDDTimeScale`、`Set-BeDDTimeScale` | `timescale` 操作 |
| `Join-BeDDServer` | `join` 操作 |
| tap 管道（`<pipeName>-tap`） | `tap` 操作的 `action:'subscribe'`：同一条 WebSocket 上的二进制消息 |
| `harness-shim.ps1`（`Click`、`Press-Key`、`Type-Text`、`Move-Mouse`、`Scroll`、`Get-ClientBitmap` 等） | 你自己基于 WebSocket 写的函数（见上）；优先用 `ui` 和 bedboo 操作 |
| `Register-BeDDNative` | 让测试框架的输入类改走 WebSocket，而不是 `SendInput` |
| `Install-BeDDMod` | `BeDD.exe mod install <实例>` |
| `Uninstall-BeDDMod` | `BeDD.exe mod uninstall <实例>`（很少需要：克隆是 Be.D.D. 自己的） |
| `Set-BeDDMode`、`-Mode` | `BeDD.exe launch --world-mode <模式>`，或 `mod config <实例> --set mode=<模式>` |
| `Set-WorldSinglePlayer` | `BeDD.exe world singleplayer <存档目录>` |
| `Start-BeDDGame.ps1`、`Start-Game` | `BeDD.exe launch [--slot N] [--label …] --detach` |
| `Stop-Game` | `BeDD.exe stop <pid>` |
| `$script:ExclusiveSignaling = $true` | `BeDD.exe launch --exclusive-signaling` |
| `clone-instance.ps1`、`New-BeDDClone` | `BeDD.exe instance clone --slot N` |
| `Initialize-BeDD.ps1`、`Initialize-BeDDInstance` | `BeDD.exe instance clone --slot N`（游戏由 LeviLauncher 准备：[新电脑](/zh/guide/new-pc)） |
| `Get-BeDDInstance` | `BeDD.exe instance show <实例>` |
| `Initialize-BeDDServer`、`Get-BeDDServer` | `BeDD.exe server init <名字>`、`server list` |
| `Start-BeDDServer.ps1`（`-Stop`、`-Command`） | `BeDD.exe server start` / `server stop`；服务器 WebSocket 上的 `command` 操作 |
| `BeDDManager.ps1`（`-List`、`-Game <pid> -Action close`、`-Shutdown`） | 双击 `BeDD.exe`；`BeDD.exe ps`、`stop <pid>`、`manager stop` |
| 配置 `pipeName`、`pipeInstances`、`managerScript` | 已移除；改用 `wsPort`、`wsMaxClients`、`manager`（[配置](/zh/guide/configuration)） |
| `BEDD_PIPE`、`BEDD_MANAGER_PIPE` | 已移除：通过 `run\<pid>.json` 找游戏 |
| `BEDD_VERSIONS_DIR`、`BEDD_LAUNCHER_DATA`、`BEDD_LIP` | 已移除：Be.D.D. 直接问 LeviLauncher（`BEDD_LEVILAUNCHER`、`BEDD_INSTANCE`） |

操作的名字、字段和回复都没变。克隆现在是 `%LOCALAPPDATA%\BeDD\instances` 下的 `<游戏版本>-<槽位>`，不再是 LeviLauncher
目录里的 `versions\<基础实例>-hi<槽位>`。

## 示例

[BeetrootSkin](https://github.com/MiracleForest/BeetrootSkin/tree/main/tools/e2e) 和
[AlleviatedBackpacks](https://github.com/MiracleForest/AlleviatedBackpacks/tree/main/tools/e2e) 有完整的测试框架
（为 Be.D.D. 1 编写；上表说明了每个调用对应什么）。
