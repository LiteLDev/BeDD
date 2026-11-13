# Integrating with your tests

Be.D.D. has no client library to load. A test harness in any language does three things: prepare and start a game
with `BeDD.exe`, talk to it over its [WebSocket](/guide/usage), and close it again.

```powershell
BeDD.exe instance clone --slot 2                         # once per run: the clone, up to date
BeDD.exe mod install 1.26.60.05-2                        # the BeDD mod and its config
# copy your own mod into (BeDD.exe instance path 1.26.60.05-2)\mods, restore your test world, then:
BeDD.exe world singleplayer <worldDir>
BeDD.exe launch --slot 2 --label my-mod --detach --json  # pid, url, token
# ... the test talks to url?token=… ...
BeDD.exe stop <pid>
```

- Every command prints only its result on stdout; `--json` makes it one JSON document, `-q` just identifiers, `-v`
  more detail on stderr. Errors print `error: …` and a `hint:` with a command to run (`{"error":{code,message,hint}}`
  with `--json`).
- Exit codes: `0` ok, `1` failure, `2` wrong usage, `4` not found, `5` conflict (e.g. the slot is in use), `10`
  declined (you said no when asked).
- Give each parallel run (worktree, CI job) its own slot. The clone's `mods\` is yours: put your mod there each run.
  Nothing has to be removed afterwards, since the clone is not the instance you play.
- `BeDD.exe dev` bundles the common steps for working on a mod; see `BeDD.exe dev --help`.

## Driving the game

Prefer the ops that do not need coordinates or pictures ([Operation modes](/guide/operation-modes)):

| Your harness wants to | Use |
|---|---|
| move, look, dig, use, chat, check the player's state | [bedboo](https://github.com/LiteLDev/bedboo) with `transport: 'bedd'`, or the ops `input`, `look`, `button`, `world` |
| click a menu button, fill a text field | `ui` (find the node by `role` and `name`, act on its `ref`) |
| press a key, type text | `key`, `text` |
| click where nothing else reaches | `mouse` (with a `ref` from `ui` when you can, `rx`,`ry` otherwise) |
| see the screen | `screenshot`, sparingly; `world` and `ui` answer most questions in text |

### From a PowerShell harness

A harness written in PowerShell can keep its scenarios and replace its input helpers with a few lines over .NET's
WebSocket client:

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

`Focus-Game` and `Assert-GameForeground` become no-ops: the game never needs the foreground.

## Moving from the PowerShell client

Be.D.D. 1 shipped a PowerShell module, a harness shim and scripts that talked to the game over a named pipe. All of
that is gone in Be.D.D. 2: the game speaks WebSocket, and `BeDD.exe` does the setup. Every old command has a
replacement:

| Be.D.D. 1 | Be.D.D. 2 |
|---|---|
| `Connect-BeDD [-PipeName p]` | read `run\<pid>.json` or `BeDD.exe ps --json`, open `url?token=…` |
| `Invoke-BeDD @{ op = … }` | send the same JSON over the WebSocket |
| `Send-BeDDKey` | op `key` |
| `Send-BeDDText` | op `text` (or bedboo `bot.chat`) |
| `Send-BeDDMouse` | op `ui` with `action:'click'` when the target is a JSON UI control, else op `mouse` |
| `Send-BeDDRaw` | op `look` (bedboo), else op `raw` |
| `Save-BeDDScreenshot`, `Get-BeDDBitmap` | op `screenshot` (base64 PNG/JPEG in the reply) |
| `Clear-BeDDInput` | op `release` |
| `Set-BeDDWindow` | op `window` |
| `Get-BeDDState` | op `state` |
| `Get-BeDDWorld` | op `world` |
| `Set-BeDDLook` | op `look` |
| `Set-BeDDInput` | op `input` |
| `Set-BeDDSlot` | op `slot` |
| `Invoke-BeDDAction` | op `action` |
| `Set-BeDDButton` | op `button` |
| `Send-BeDDPacket` | op `send` |
| `Send-BeDDStack` | op `stack` |
| `Get-BeDDTimeScale`, `Set-BeDDTimeScale` | op `timescale` |
| `Join-BeDDServer` | op `join` |
| the tap pipe (`<pipeName>-tap`) | op `tap` with `action:'subscribe'`: binary messages on the same WebSocket |
| `harness-shim.ps1` (`Click`, `Press-Key`, `Type-Text`, `Move-Mouse`, `Scroll`, `Get-ClientBitmap`, …) | your own helpers over the WebSocket (above); prefer `ui` and bedboo ops |
| `Register-BeDDNative` | point the harness's input class at the WebSocket instead of `SendInput` |
| `Install-BeDDMod` | `BeDD.exe mod install <instance>` |
| `Uninstall-BeDDMod` | `BeDD.exe mod uninstall <instance>` (rarely needed: clones are Be.D.D.'s own) |
| `Set-BeDDMode`, `-Mode` | `BeDD.exe launch --world-mode <mode>`, or `mod config <instance> --set mode=<mode>` |
| `Set-WorldSinglePlayer` | `BeDD.exe world singleplayer <worldDir>` |
| `Start-BeDDGame.ps1`, `Start-Game` | `BeDD.exe launch [--slot N] [--label …] --detach` |
| `Stop-Game` | `BeDD.exe stop <pid>` |
| `$script:ExclusiveSignaling = $true` | `BeDD.exe launch --exclusive-signaling` |
| `clone-instance.ps1`, `New-BeDDClone` | `BeDD.exe instance clone --slot N` |
| `Initialize-BeDD.ps1`, `Initialize-BeDDInstance` | `BeDD.exe instance clone --slot N` (LeviLauncher sets up the game: [new PC](/guide/new-pc)) |
| `Get-BeDDInstance` | `BeDD.exe instance show <instance>` |
| `Initialize-BeDDServer`, `Get-BeDDServer` | `BeDD.exe server init <name>`, `server list` |
| `Start-BeDDServer.ps1` (`-Stop`, `-Command`) | `BeDD.exe server start` / `server stop`; op `command` on the server's WebSocket |
| `BeDDManager.ps1` (`-List`, `-Game <pid> -Action close`, `-Shutdown`) | double-click `BeDD.exe`; `BeDD.exe ps`, `stop <pid>`, `manager stop` |
| config `pipeName`, `pipeInstances`, `managerScript` | gone; `wsPort`, `wsMaxClients`, `manager` ([Configuration](/guide/configuration)) |
| `BEDD_PIPE`, `BEDD_MANAGER_PIPE` | gone: games are found through `run\<pid>.json` |
| `BEDD_VERSIONS_DIR`, `BEDD_LAUNCHER_DATA`, `BEDD_LIP` | gone: Be.D.D. asks LeviLauncher (`BEDD_LEVILAUNCHER`, `BEDD_INSTANCE`) |

The ops keep their names, fields and replies. Clones are now
`<gameVersion>-<slot>` under `%LOCALAPPDATA%\BeDD\instances` instead of `versions\<base>-hi<slot>` inside
LeviLauncher's folder.

## Examples

[BeetrootSkin](https://github.com/MiracleForest/BeetrootSkin/tree/main/tools/e2e) and
[AlleviatedBackpacks](https://github.com/MiracleForest/AlleviatedBackpacks/tree/main/tools/e2e) have complete harnesses
(written for Be.D.D. 1; the table above shows what each call becomes).
