# Integrating with your tests

No client library: run `BeDD.exe`, then drive the [WebSocket](/guide/usage), bedboo first
([order](/guide/operation-modes#which-one-to-use)).

```powershell
BeDD.exe instance clone --slot 2                   # then copy your mod into (BeDD.exe instance path 1.26.60.05-2)\mods
BeDD.exe world singleplayer <worldDir>             # after restoring your test world
BeDD.exe launch --slot 2 --label my-mod --json     # pid, url, token
BeDD.exe stop <pid>
```

```js
const bot = bedboo.createBot({ transport: 'bedd', bedd: { url: game.url, token: game.token } });
```

- stdout: the result only; `--json` one document, `-q` identifiers, `-v` detail on stderr. Errors: `error: …` +
  `hint:` (`{"error":{code,message,hint}}`).
- Exit codes: `0` ok, `1` failure, `2` usage, `4` not found, `5` conflict (e.g. slot in use), `10` declined.
- One slot per parallel run; nothing to clean up.
- Your own BeDD build: `mod install <instance> --from <dir>` ([pin](/guide/installation#upgrading)).
- `BeDD.exe dev`: common mod-development steps (`--help`).

PowerShell: `[System.Net.WebSockets.ClientWebSocket]`, one UTF-8 text message per op, read until `EndOfMessage`.
`Focus-Game`, `Assert-GameForeground`: no-ops.

## Moving from the PowerShell client

The module, shim and named pipe are gone; ops are unchanged. Clones moved from LeviLauncher's
`versions\<base>-hi<slot>` to `instances\<gameVersion>-<slot>`.

| Be.D.D. 1 | Be.D.D. 2 |
|---|---|
| `Connect-BeDD [-PipeName p]` | `BeDD.exe ps --json` or `run\<pid>.json`; open `url?token=…` |
| `Invoke-BeDD @{ op = … }` | the same JSON over the WebSocket |
| `Get-BeDDState`, `Get-BeDDWorld`, `Set-BeDDLook`, `Set-BeDDInput`, `Set-BeDDSlot`, `Invoke-BeDDAction`, `Set-BeDDButton`, `Send-BeDDPacket`, `Send-BeDDStack`, `Get-`/`Set-BeDDTimeScale`, `Join-BeDDServer`, `Send-BeDDKey`, `Clear-BeDDInput`, `Set-BeDDWindow` | op `state`, `world`, `look`, `input`, `slot`, `action`, `button`, `send`, `stack`, `timescale`, `join`, `key`, `release`, `window` |
| `Send-BeDDText`, `Send-BeDDMouse`, `Send-BeDDRaw` | `bot.chat` / `ui` click on a `ref` / `look`; else op `text` / `mouse` / `raw` |
| `Save-BeDDScreenshot`, `Get-BeDDBitmap` | op `screenshot` |
| tap pipe `<pipeName>-tap` | op `tap` `action:'subscribe'` (binary messages) |
| `harness-shim.ps1` (`Click`, `Press-Key`, `Type-Text`, `Move-Mouse`, `Scroll`, `Get-ClientBitmap`, …) | your own helpers over the WebSocket; prefer bedboo and `ui` |
| `Register-BeDDNative` | point the input class at the WebSocket instead of `SendInput` |
| `Install-BeDDMod`, `Uninstall-BeDDMod` | `BeDD.exe mod install`, `mod uninstall <instance>` |
| `Set-BeDDMode`, `-Mode` | `BeDD.exe mod config <instance> --world-mode <mode>` |
| `Set-WorldSinglePlayer` | `BeDD.exe world singleplayer <worldDir>` |
| `Start-BeDDGame.ps1`, `Start-Game`; `Stop-Game` | `BeDD.exe launch [--slot N] [--label …] --json`; `stop <pid>` |
| `$script:ExclusiveSignaling = $true` | `launch --exclusive-signaling` |
| `clone-instance.ps1`, `New-BeDDClone`, `Initialize-BeDD.ps1`, `Initialize-BeDDInstance` | `BeDD.exe instance clone --slot N` ([new PC](/guide/new-pc)) |
| `Get-BeDDInstance`, `Initialize-BeDDServer`, `Get-BeDDServer` | `BeDD.exe instance show`, `server init`, `server list` |
| `Start-BeDDServer.ps1` (`-Stop`, `-Command`) | `server start` / `server stop`; op `command` on the server |
| `BeDDManager.ps1` (`-List`, `-Game <pid> -Action close`, `-Shutdown`) | `BeDD.exe` (window), `ps`, `stop <pid>`, `manager stop` |
| config `pipeName`, `pipeInstances`, `managerScript` | `wsPort`, `wsMaxClients`, `manager` |
| `BEDD_PIPE`, `BEDD_MANAGER_PIPE`; `BEDD_VERSIONS_DIR`, `BEDD_LAUNCHER_DATA`, `BEDD_LIP` | `run\<pid>.json`; `BEDD_LEVILAUNCHER`, `BEDD_INSTANCE` |

Be.D.D. 1 harnesses: [BeetrootSkin](https://github.com/MiracleForest/BeetrootSkin/tree/main/tools/e2e),
[AlleviatedBackpacks](https://github.com/MiracleForest/AlleviatedBackpacks/tree/main/tools/e2e).
