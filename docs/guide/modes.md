# Where the world runs

Be.D.D. debugs a client mod in one of three world modes. This is about where the world runs, not about how your test
drives the game (that is the [operation mode](/guide/operation-modes)).

| World mode | What runs | Use it for |
|---|---|---|
| **Client only** (`ClientOnly`, default) | a game with the BeDD mod, in a singleplayer or LAN world | most client mod tests |
| **Client + server** (`ClientServer`) | a game with BeDD and a Bedrock Dedicated Server (BDS) with BeDD's server mod, on the same PC | client behaviour against a real server, both sides scripted; the server's game speed leads the client's |
| **Client → remote** (`Remote`) | a game with BeDD joined to a server elsewhere | a server you do not run (BeDD is only in the client) |

Pick it per run with `BeDD.exe launch --world-mode <mode>`, or once per clone with
`BeDD.exe mod config <instance> --set mode=<mode>`. The client side is the same in all three:
[Driving the game](/guide/usage), [Several games at once](/guide/several-games). This page is about the server.

## Client + server

```powershell
BeDD.exe server init s1                     # %LOCALAPPDATA%\BeDD\servers\s1, made ready
BeDD.exe server start s1 --json             # up in the background: pid, port, url, token
BeDD.exe launch --slot 1 --world-mode ClientServer --detach
# ... join 127.0.0.1:<port> from the game (the join op), drive both sides ...
BeDD.exe server stop s1                     # saves the world and exits
```

The server has a WebSocket of its own, found the same way as a game's (`run\<pid>.json` with `"kind": "server"`,
`BeDD.exe ps --json`). It answers `ping`, `state`, `server` (tick, ticks per second, port, players), `timescale` and
`command`:

```js
await server('command', { command: 'time set day' });   // as the server console would; replies the output lines
await server('timescale', { scale: 3 });                 // the game speed of the server, which its players follow
```

### Setting up a server

`server init` installs LeviLamina's server package with lip (downloaded to `%LOCALAPPDATA%\BeDD\tools`) into an empty
folder; that package brings the official BDS of Minecraft 1.26.60 with it (downloaded from minecraft.net, about
100 MB), and puts BeDD's server mod into `mods\BeDD-server`. It skips whatever is already done, runs one setup at a
time on the PC and never asks anything, so harnesses and CI can call it before every run.

| Flag | |
|---|---|
| `<name>` | the server, a folder under `%LOCALAPPDATA%\BeDD\servers` (`BEDD_SERVERS_DIR`); default `server` |
| `--port`, `--port-v6` | default: the server's ports from before, else the first free pair of 19140/19141, 19142/19143, …, so servers never meet a game's 19132 or each other |
| `--transport` | default `nethernet`: since 1.26.60 the game joins servers over NetherNet only (a `raknet` server fails with "Terracotta"). The server signals on TCP `server-port` and carries the game over UDP; with `--server-ip 127.0.0.1` (the default) it publishes `127.0.0.1:<port>` for that (`server-udp-ports`), since BeDD keeps both sides' sockets on loopback. The first join to a server asks "Trust this server?" |
| `--server-ip` | default `127.0.0.1`: only games on this PC join; `''` listens on every interface |
| `--online-mode` | games must sign in with Xbox to join (default off) |
| `--property key=value` | any `server.properties` key, repeatable: `level-name`, `gamemode` (default `creative`), `difficulty` (`peaceful`), `level-seed`, `max-players`, `allow-cheats` (`true`), `view-distance`, … |
| `--set key=value` | a key of the server mod's `config\config.json`, repeatable: `timeScale` (the game speed it starts with), `trace`, … |

`BeDD.exe server list` shows every server with its folder, ports and whether it runs; `server remove <name>`
deletes one.

### Starting and stopping

`server start <name>` starts `bedrock_server_mod.exe` (the BDS with LeviLamina) with no window at all: nothing shows
up on the desktop or takes the focus. Everything the server prints goes to `logs\console.log` in its folder. It waits
until the server is up and its WebSocket listens, and prints `pid`, `port`, `portV6`, `url`, `token`, `dir` and
`log`. A server that already runs is reported as it is. `--timeout <s>` is how long to wait (180).

`server stop <name>` sends `stop` (the server saves the world and exits), and kills it after 30 s.

### No firewall prompts

A BDS listens on every network interface, and the first time a program does that Windows Firewall asks on the desktop
whether to allow it, once per server folder, which would stop an unattended run. BeDD's server mod binds every
socket of the server to the loopback address instead (config `loopbackOnly`, default on, as in the client), so Windows
never asks, and only games on this PC can join. `server init` also sets `server-ip=127.0.0.1` and turns LAN
visibility off, which would otherwise bind Bedrock's default ports as well.

So `server start` refuses to start a server that has no BeDD server mod in `mods\BeDD-server`, has another mod there,
or has `"loopbackOnly": false` in its `config.json`, and says which. Should LeviLamina still fail to load the mod, it
stops the server before the server opens its network. `--allow-firewall-prompt` starts it anyway, for a server that
games on other PCs should join (with `--server-ip ''`): then allow it when Windows asks.

### Several servers

Each server has its own folder, ports and WebSocket, so runs in parallel each start their own: give each a name of
its own (e.g. the slot of its game) and leave `--port` out.

## Client → remote

```powershell
BeDD.exe mod config 1.26.60.05-1 --set 'allowHosts=["play.example.net"]'
BeDD.exe launch --slot 1 --world-mode Remote --exclusive-signaling --detach
```

The game stays on loopback for everything except the hosts in `allowHosts`
([Configuration](/guide/configuration#no-firewall-prompts)). A server that needs an online sign-in also needs the PC
to itself while it plays: `--exclusive-signaling` ([Rules for parallel runs](/guide/several-games#rules-for-parallel-runs)).
