# Configuration

The mod reads `mods\BeDD\config\config.json` in its clone. `BeDD.exe mod install` writes it with the defaults; change
keys with `mod config`, never by hand-editing a file inside a clone (its files may be hardlinks shared with the base):

```powershell
BeDD.exe mod config 1.26.60.05-2                                      # print it
BeDD.exe mod config 1.26.60.05-2 --set clientWidth=1600 --set clientHeight=900
BeDD.exe mod config 1.26.60.05-2 --set 'allowHosts=["play.example.net"]' --set manager=false
```

A value is read as JSON when it parses as JSON (`true`, `8`, `["a"]`) and as a string otherwise. Without a config
file the game runs with the defaults.

`BeDD.exe mod install` and `launch` also switch the clone's LeviLamina to its built-in crash logger
(`modules.crashLogger.builtin` in `mods\LeviLamina\config\Config.json`): the default `CrashLogger.exe` is a console
program, and its console window would flash on the desktop at every game start.

## Keys

| Key | |
|---|---|
| `clientWidth`, `clientHeight` | game window size (0 = leave it), so clicks land in the same place every run |
| `wsPort` | default `0`: the WebSocket's port on `127.0.0.1`; 0 picks a free one, which `run\<pid>.json` reports. Set a fixed port only for one game at a time |
| `wsMaxClients` | default `8`: WebSocket connections the game serves at once (each one's held keys, buttons and `input` override are released when it closes) |
| `allowOrigins` | default `[]`: web page origins (`"http://localhost:5173"`) allowed to connect. Any request with an `Origin` header not listed here gets 403, so a web page you happen to open cannot reach the game |
| `manager` | default `true`: the game shows up in the [manager window](/guide/several-games#the-manager-window) and closes when none attaches within 30 s or it loses it. `false` for headless CI runs without the window: the game stays hidden and never closes by itself |
| `loopbackOnly` | default `true`: the game stays on the PC, so no Windows Firewall prompt for each new clone, and game servers outside the PC are not queried (they could show up as "LAN worlds" ahead of your test world). In BeDD's server mod it is on by default too and keeps the server on loopback (its IPv4/IPv6 ports, LAN discovery and NetherNet), so no prompt; `state` reports it. Its port is then reachable at `127.0.0.1` only |
| `mode` | where the world runs: `ClientOnly` (default), `ClientServer` or `Remote` ([world modes](#world-modes)); `launch --world-mode` sets it for one run. `state` and `join` report it |
| `allowHosts` | host names or addresses `loopbackOnly` still lets the game reach (the server of the Remote world mode). Those hosts are queried, the game's outgoing UDP socket can reach them without a firewall prompt, and datagrams to any other host off the PC are refused (`state` `stats.sendsRefused`). Hosts on the PC need no entry |
| `mute` | ignored: a BeDD game is always silent (FMOD opens no audio device); `false` only logs a warning |
| `trace` | log every input that reaches the game to `logs\latest.log`, for bug reports |
| `timeScale` | default `1`: the game speed (0.05..20) of every world this game hosts, set when it enters one (the `timescale` op changes it later). On a server with BeDD the server's speed leads and the client follows it; on a server without BeDD it does nothing. In BeDD's server mod: applied once the server has started and sent to its players |

## Feature keys

These turn the [operation modes](/guide/operation-modes) and their riskier ops on and off. All are **on** by
default: the WebSocket only listens on `127.0.0.1` and wants the token, which only your user can read from
`%LOCALAPPDATA%\BeDD\run`. An op whose key is `false` answers `{"ok":false,"code":"disabled","key":"<key>"}`.

| Key | |
|---|---|
| `gameHooks` | default `true`: hook the game's own systems for the bedboo ops `world`, `look`, `input` (walk, jump, sneak and sprint whatever the key bindings), `slot`, `action` and `button` (hotbar, drop/inventory/perspective/pause, attack/use/pick held for game ticks or until the targeted block breaks) |
| `tap` | default `true`: the `tap` op streams the game packets the client receives and sends as binary WebSocket messages ([Tap protocol](/reference/tap)), with a join journal so a reader can attach in the middle of a session. It carries the whole session of the signed-in player. While it is on the client asks servers not to use its blob cache, so every chunk arrives in full |
| `tapLoopback` | default `true`, used only with `tap`: also tap singleplayer worlds and LAN worlds this game hosts (packets re-serialized; LAN guests' traffic is left out) |
| `tapJournalMiB` | default `128`: size cap of the tap's join journal; past it a late attach is incomplete (`complete:false`) |
| `allowSend` | default `true`: the `send` op, with which a script such as bedboo makes the client send chat and commands it serialized |
| `sendAllow` | packet ids `send` may send, default `[9, 77, 78, 97]` (text, command_request, command_block_update, book_edit: what the player can type or write in the game's own screens); login, inventory_transaction, player_auth_input and item_stack_request are always refused |
| `allowStack` | default `true`: the `stack` op lets a script make the game send item stack requests (inventory moves, crafting, chest moves) for the signed-in player, as bedboo does |
| `ui` | default `true`: the `ui` op, the [UI tree](/guide/operation-modes#ui-tree) of the game's screens |
| `uiMaxNodes` | default `1000`: the most nodes one `ui` reply holds (a request's `maxNodes` can only lower it); `truncated` says when nodes were left out |

Turn a key off for a run that must not do that, e.g. `allowSend=false` when a test checks that chat goes through
the chat screen. See [Security](/reference/websocket#security) for what each key lets a WebSocket client do.

## World modes

A BeDD game runs headless in every mode; the mode is about where the world runs ([Where the world runs](/guide/modes)).

| Mode | The world runs | Network |
|---|---|---|
| `ClientOnly` (default) | in the client: a local world | loopback only |
| `ClientServer` | in a dedicated server with BeDD on this PC; its game speed drives the client's | loopback only: join it at `127.0.0.1` |
| `Remote` | in a server without BeDD, possibly off the PC; the client cannot change its game speed | loopback only, except for `allowHosts` |

The `join` op joins a server in any mode, from the title screen or from inside a world
([Driving the game](/guide/usage#servers)).

### No firewall prompts

A BeDD game never makes Windows Firewall ask "allow access?": the firewall asks the first time a program (by full exe
path, so every clone anew) binds a socket explicitly to an address other than loopback. With `loopbackOnly` every
wildcard bind goes to loopback instead, in all three modes; the WebSocket listens on `127.0.0.1` too. In Remote mode,
`allowHosts` changes one thing: the client's outgoing UDP socket (a wildcard bind to port 0) is not bound explicitly
at all but connected towards the allowed server, which binds it implicitly (the firewall does not ask for that);
listening sockets (LAN hosting, LAN discovery) stay on loopback, and datagrams to other hosts off the PC are refused.
Do not turn `loopbackOnly` off to reach a remote server: every new clone would prompt again. A dedicated server you
start yourself is another program: one without BeDD's loopback binding asks once per server folder.

## Environment variables

| Variable | Read by | |
|---|---|---|
| `BEDD_HOME` | `BeDD.exe`, the mod | Be.D.D.'s folder (default `%LOCALAPPDATA%\BeDD`): clones, `run`, `locks`, `servers`, `tools`, `logs` ([Setting up a new PC](/guide/new-pc#folders)) |
| `BEDD_INSTANCES_DIR` | `BeDD.exe` | where clones go; put it on the base instance's volume so clones are hardlinked, not copied |
| `BEDD_SERVERS_DIR` | `BeDD.exe` | where the dedicated servers of [client + server runs](/guide/modes) live (default `%LOCALAPPDATA%\BeDD\servers`) |
| `BEDD_LEVILAUNCHER` | `BeDD.exe` | the `LeviLauncher.exe` whose instances Be.D.D. reads |
| `BEDD_INSTANCE` | `BeDD.exe` | the base instance, when LeviLauncher has several of 1.26.60 (`--base` on the command line) |
| `BEDD_LABEL` | the mod | the game's name in the manager window (`launch --label` sets it) |
| `BEDD_RETURN_FOCUS` | the mod | a window handle that gets the foreground back if the game still activates itself while starting (`launch` sets it to the window that was in front) |
| `BEDD_DIR` | your harness | where Be.D.D. is unpacked ([Installation](/guide/installation#let-the-harness-fetch-it)) |
