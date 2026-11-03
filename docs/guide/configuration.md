# Configuration

The module reads `mods\BeDD\config\config.json`, usually written by the harness (`Install-BeDDMod` does it).
Without it the game still runs headless, with the defaults and no `managerScript` (so only in a manager window that
already runs).

```json
{ "clientWidth": 1600, "clientHeight": 900, "pipeName": "BeDD", "managerScript": "C:\\BeDD\\BeDDManager.ps1" }
```

`Install-BeDDMod` and `Start-BeDDGame.ps1` also switch the instance's LeviLamina to its built-in crash logger
(`modules.crashLogger.builtin` in `mods\LeviLamina\config\Config.json`): the default `CrashLogger.exe` is a console
program, and its console window would flash on the desktop at every game start.

## Keys

| Key | |
|---|---|
| `clientWidth`, `clientHeight` | game window size (0 = leave it), so clicks land in the same place every run |
| `pipeName` | one per game instance when several run at once |
| `loopbackOnly` | default `true`: the game stays on the PC, so no Windows Firewall prompt for each new instance, and game servers outside the PC are not queried (they could show up as "LAN worlds" ahead of your test world). Dedicated servers your harness starts still ask once per server folder (**Cancel** is fine), unless they run BeDD's server target: there it is on by default too and keeps the server on loopback (its IPv4/IPv6 ports, LAN discovery and NetherNet), so no prompt; `state` reports it. Its port is then reachable at `127.0.0.1` only |
| `mode` | where the world runs: `ClientOnly` (default: a local world), `ClientServer` (a dedicated server with BeDD on this PC; `loopbackOnly` stays), `Remote` (a server without BeDD, possibly off the PC). See [Debug modes](/guide/configuration#debug-modes). `state` and `join` report it |
| `allowHosts` | host names or addresses `loopbackOnly` still lets the game reach (Remote mode's server; `Start-BeDDGame.ps1 -Mode Remote -ServerHost` writes it). Those hosts are queried, the game's outgoing UDP socket can reach them without a firewall prompt, and datagrams to any other host off the PC are refused (`state` `stats.sendsRefused`). Hosts on the PC need no entry |
| `managerScript` | path of `BeDDManager.ps1` (`Install-BeDDMod` writes it): the game starts the [manager window](/guide/several-games#the-manager-window) with it when none runs |
| `mute` | default `true`: the game makes no sound |
| `trace` | log every input that reaches the game to `logs\latest.log`, for bug reports |
| `pipeInstances` | default `4`: scripts that may use the game's pipe at once (each one's held keys, buttons and `input` override are released when it disconnects); `1` = one at a time |
| `timeScale` | default `1`: the game speed (0.05..20) of every world this game hosts, set when it enters one (`Set-BeDDTimeScale` changes it later). On a server with BeDD the server's speed leads and the client follows it; on a server without BeDD it does nothing. In BeDD's server target: applied once the server has started and sent to its players |

## Debug modes

A BeDD game runs headless in every mode; the mode is about where the world runs. `Start-BeDDGame.ps1 -Mode` and
`Install-BeDDMod -Mode` write it (`Set-BeDDMode`); without `-Mode`, `Start-BeDDGame.ps1` leaves the config as it is.

| Mode | The world runs | Network |
|---|---|---|
| `ClientOnly` (default) | in the client: a local world, as before | loopback only |
| `ClientServer` | in a dedicated server with BeDD on this PC; its game speed drives the client's | loopback only: join it at `127.0.0.1` |
| `Remote` | in a server without BeDD (`-ServerHost`), possibly off the PC; the client cannot change its game speed | loopback only, except for `-ServerHost` (`allowHosts`) |

`Join-BeDDServer -Host 127.0.0.1 -Port 19132 -Wait` joins a server in any mode, from the title screen or from inside a
world ([Driving the game](/guide/usage#servers)).

### No firewall prompts

A BeDD game never makes Windows Firewall ask "allow access?": the firewall asks the first time a program (by full exe
path, so every clone instance anew) binds a socket explicitly to an address other than loopback. With `loopbackOnly`
every wildcard bind goes to loopback instead, in all three modes. In Remote mode, `allowHosts` changes one thing: the
client's outgoing UDP socket (a wildcard bind to port 0) is not bound explicitly at all but connected towards the
allowed server, which binds it implicitly (the firewall does not ask for that); listening sockets (LAN hosting, LAN
discovery) stay on loopback, and datagrams to other hosts off the PC are refused. Do not turn `loopbackOnly` off to
reach a remote server: every new instance would prompt again. A dedicated server you start yourself is another
program: one without BeDD's loopback binding asks once per server folder.

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
| `BEDD_MANAGER_PIPE` | `BeDDManager.ps1`, the module | the pipe of another manager window than the session's, e.g. a recording's own; games started with it set show up there |
| `BEDD_LABEL` | the module | the game's name in the manager window (`Start-BeDDGame.ps1 -Label` sets it) |
| `BEDD_RETURN_FOCUS` | the module | a window handle that gets the foreground back if the game still activates itself while starting (`Start-BeDDGame.ps1` and the shim's `Start-Game` set it) |
| `BEDD_DIR` | your harness | where Be.D.D. is unpacked ([Installation](/guide/installation#let-the-harness-fetch-it)) |
| `BEDD_INSTANCE`, `BEDD_VERSIONS_DIR`, `BEDD_LAUNCHER_DATA`, `BEDD_LEVILAUNCHER`, `BEDD_LIP` | `BeDDSetup.psm1` | where the game instance and tools are ([Setting up a new PC](/guide/new-pc#which-instance)) |
| `BEDD_SERVERS_DIR` | `BeDDSetup.psm1`, `Start-BeDDServer.ps1` | where the dedicated servers of [client + server mode](/guide/modes) live (default `%LOCALAPPDATA%\BeDD\servers`) |
