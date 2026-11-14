# 接入你的测试

没有客户端库：运行 `BeDD.exe`，再驱动 [WebSocket](/zh/guide/usage)，先用 bedboo
（[顺序](/zh/guide/operation-modes#用哪一种)）。

```powershell
BeDD.exe instance clone --slot 2                   # then copy your mod into (BeDD.exe instance path 1.26.60.05-2)\mods
BeDD.exe world singleplayer <worldDir>             # after restoring your test world
BeDD.exe launch --slot 2 --label my-mod --json     # pid, url, token
BeDD.exe stop <pid>
```

```js
const bot = bedboo.createBot({ transport: 'bedd', bedd: { url: game.url, token: game.token } });
```

- stdout 只有结果；`--json` 输出一个文档，`-q` 输出标识符，`-v` 在 stderr 输出细节。出错：`error: …` +
  `hint:`（`{"error":{code,message,hint}}`）。
- 退出码：`0` 成功，`1` 失败，`2` 用法错误，`4` 找不到，`5` 冲突（如槽位正在使用），`10` 被拒绝。
- 每个并行运行一个槽位；无需清理。
- 你自己构建的 BeDD：`mod install <instance> --from <dir>`（[固定来源](/zh/guide/installation#升级)）。
- `BeDD.exe dev`：开发模组的常用步骤（`--help`）。

PowerShell：`[System.Net.WebSockets.ClientWebSocket]`，每个操作一条 UTF-8 文本消息，读到 `EndOfMessage` 为止。
`Focus-Game`、`Assert-GameForeground`：空操作。

## 从 PowerShell 客户端迁移

模块、垫片和命名管道都没有了；操作不变。克隆从 LeviLauncher 的 `versions\<base>-hi<slot>` 移到
`instances\<gameVersion>-<slot>`。

| Be.D.D. 1 | Be.D.D. 2 |
|---|---|
| `Connect-BeDD [-PipeName p]` | `BeDD.exe ps --json` 或 `run\<pid>.json`；打开 `url?token=…` |
| `Invoke-BeDD @{ op = … }` | 通过 WebSocket 发同样的 JSON |
| `Get-BeDDState`、`Get-BeDDWorld`、`Set-BeDDLook`、`Set-BeDDInput`、`Set-BeDDSlot`、`Invoke-BeDDAction`、`Set-BeDDButton`、`Send-BeDDPacket`、`Send-BeDDStack`、`Get-`/`Set-BeDDTimeScale`、`Join-BeDDServer`、`Send-BeDDKey`、`Clear-BeDDInput`、`Set-BeDDWindow` | 操作 `state`、`world`、`look`、`input`、`slot`、`action`、`button`、`send`、`stack`、`timescale`、`join`、`key`、`release`、`window` |
| `Send-BeDDText`、`Send-BeDDMouse`、`Send-BeDDRaw` | `bot.chat` / 对 `ref` 的 `ui` click / `look`；否则操作 `text` / `mouse` / `raw` |
| `Save-BeDDScreenshot`、`Get-BeDDBitmap` | 操作 `screenshot` |
| tap 管道 `<pipeName>-tap` | 操作 `tap` `action:'subscribe'`（二进制消息） |
| `harness-shim.ps1`（`Click`、`Press-Key`、`Type-Text`、`Move-Mouse`、`Scroll`、`Get-ClientBitmap` 等） | 你自己基于 WebSocket 的函数；优先用 bedboo 和 `ui` |
| `Register-BeDDNative` | 让输入类改走 WebSocket，而不是 `SendInput` |
| `Install-BeDDMod`、`Uninstall-BeDDMod` | `BeDD.exe mod install`、`mod uninstall <instance>` |
| `Set-BeDDMode`、`-Mode` | `BeDD.exe mod config <instance> --world-mode <mode>` |
| `Set-WorldSinglePlayer` | `BeDD.exe world singleplayer <worldDir>` |
| `Start-BeDDGame.ps1`、`Start-Game`；`Stop-Game` | `BeDD.exe launch [--slot N] [--label …] --json`；`stop <pid>` |
| `$script:ExclusiveSignaling = $true` | `launch --exclusive-signaling` |
| `clone-instance.ps1`、`New-BeDDClone`、`Initialize-BeDD.ps1`、`Initialize-BeDDInstance` | `BeDD.exe instance clone --slot N`（[新电脑](/zh/guide/new-pc)） |
| `Get-BeDDInstance`、`Initialize-BeDDServer`、`Get-BeDDServer` | `BeDD.exe instance show`、`server init`、`server list` |
| `Start-BeDDServer.ps1`（`-Stop`、`-Command`） | `server start` / `server stop`；服务器上的 `command` 操作 |
| `BeDDManager.ps1`（`-List`、`-Game <pid> -Action close`、`-Shutdown`） | `BeDD.exe`（窗口）、`ps`、`stop <pid>`、`manager stop` |
| 配置 `pipeName`、`pipeInstances`、`managerScript` | `wsPort`、`wsMaxClients`、`manager` |
| `BEDD_PIPE`、`BEDD_MANAGER_PIPE`；`BEDD_VERSIONS_DIR`、`BEDD_LAUNCHER_DATA`、`BEDD_LIP` | `run\<pid>.json`；`BEDD_LEVILAUNCHER`、`BEDD_INSTANCE` |

Be.D.D. 1 的测试框架：[BeetrootSkin](https://github.com/MiracleForest/BeetrootSkin/tree/main/tools/e2e)、
[AlleviatedBackpacks](https://github.com/MiracleForest/AlleviatedBackpacks/tree/main/tools/e2e)。
