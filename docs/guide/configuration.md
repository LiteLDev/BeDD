# Configuration

The module reads `mods\BeDD\config\config.json`, usually written by the harness (`Install-BeDDMod` does it).
Without it the module stays passive: background mode off, the pipe still answers.

```json
{ "background": true, "placement": "back", "clientWidth": 1600, "clientHeight": 900, "pipeName": "BeDD" }
```

`Install-BeDDMod` and `Start-BeDDGame.ps1` also switch the instance's LeviLamina to its built-in crash logger
(`modules.crashLogger.builtin` in `mods\LeviLamina\config\Config.json`): the default `CrashLogger.exe` is a console
program, and its console window would flash on the desktop at every game start.

## Keys

| Key | |
|---|---|
| `background` | keep the game in the background and away from your real mouse and keyboard |
| `placement` | `back` (behind your windows), `offscreen` or `none` |
| `clientWidth`, `clientHeight` | game window size (0 = leave it), so clicks land in the same place every run |
| `pipeName` | one per game instance when several run at once |
| `loopbackOnly` | default `true`: the game stays on the PC, so no Windows Firewall prompt for each new instance, and game servers outside the PC are not queried (they could show up as "LAN worlds" ahead of your test world). Dedicated servers your harness starts still ask once per server folder; **Cancel** is fine |
| `manager` | default `true`: in background mode the game shows up in the manager window when one runs, and its own window stays off screen and off the taskbar |
| `mute` | default `true`: in background mode the game makes no sound (it is audible again while you take it over) |
| `trace` | log every input that reaches the game to `logs\latest.log`, for bug reports |
| `pipeInstances` | default `4`: scripts that may use the game's pipe at once (each one's held keys, buttons and `input` override are released when it disconnects); `1` = one at a time |

## Keys for bedboo

These turn on the ops that [bedboo](https://github.com/LiteLDev/bedboo)'s transport `bedd` uses. All are off by
default; keep them off unless a harness needs them (see [Security](/reference/protocol#security)).

| Key | |
|---|---|
| `gameHooks` | default `false`: hook the game's own systems for `world` and `look` (`Get-BeDDWorld`, `Set-BeDDLook`), `input` (`Set-BeDDInput`: walk, jump, sneak and sprint whatever the key bindings), `slot`, `action` and `button` (`Set-BeDDSlot`, `Invoke-BeDDAction`, `Set-BeDDButton`: hotbar, drop/inventory/perspective/pause, attack/use/pick held for game ticks or until the targeted block breaks) |
| `tap` | default `false`: serve `\\.\pipe\<pipeName>-tap`, a binary stream of the game packets the client receives and sends ([Tap protocol](/reference/tap)), with a join journal so a reader can attach in the middle of a session. It carries the whole session of the signed-in player, so only your user can open it. While it is on the client asks servers not to use its blob cache, so every chunk arrives in full |
| `tapLoopback` | default `true`, used only with `tap`: also tap singleplayer worlds and LAN worlds this game hosts (packets re-serialized; LAN guests' traffic is left out) |
| `tapJournalMiB` | default `128`: size cap of the tap's join journal; past it a late attach is incomplete (`complete:false`) |
| `allowSend` | default `false`: allow the `send` pipe op (`Send-BeDDPacket`), with which a script such as bedboo makes the client send chat and commands it serialized (any local program of yours could then do so as your player) |
| `sendAllow` | packet ids `send` may send, default `[9, 77]` (text, command_request); login, inventory_transaction, player_auth_input and item_stack_request are always refused |
| `allowStack` | default `false`: the `stack` op (`Send-BeDDStack`) lets a pipe client make the game send item stack requests (inventory moves, crafting, chest moves) for the signed-in player, as bedboo does in bedd mode. Turn it on only for such harnesses |

## Environment variables

| Variable | Read by | |
|---|---|---|
| `BEDD_PIPE` | `BeDD.psm1` | the pipe `Connect-BeDD` uses without `-PipeName` (default `BeDD`) |
| `BEDD_MANAGER` | `Start-BeDDGame.ps1`, the shim | `0`: no manager window |
| `BEDD_LABEL` | the module | the game's name in the manager window (`Start-BeDDGame.ps1 -Label` sets it) |
| `BEDD_RETURN_FOCUS` | the module | a window handle that gets the foreground back if the game still activates itself while starting (`Start-BeDDGame.ps1` and the shim's `Start-Game` set it) |
| `BEDD_DIR` | your harness | where Be.D.D. is unpacked ([Installation](/guide/installation#let-the-harness-fetch-it)) |
| `BEDD_INSTANCE`, `BEDD_VERSIONS_DIR`, `BEDD_LAUNCHER_DATA`, `BEDD_LEVILAUNCHER`, `BEDD_LIP` | `BeDDSetup.psm1` | where the game instance and tools are ([Setting up a new PC](/guide/new-pc#which-instance)) |
