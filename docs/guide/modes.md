# Where the world runs

Config `mode`; not to be confused with the [operation mode](/guide/operation-modes).

| World mode | What runs | Use |
|---|---|---|
| `ClientOnly` (default) | the game with BeDD, singleplayer or LAN world; loopback only | most client mod tests |
| `ClientServer` | the game + a Bedrock Dedicated Server (BDS) with BeDD's server mod at `127.0.0.1` | both sides scripted; the server's speed leads |
| `Remote` | the game joined to a server without BeDD, reachable via `allowHosts`; speed not settable | servers you do not run |

Set it per clone before `launch`: `BeDD.exe mod config <instance> --world-mode <mode>` (also `mod install`). `Remote`
needs `--server-host <host>`, added to `allowHosts` when off the PC.

## Client + server

```powershell
BeDD.exe server init s1                     # %LOCALAPPDATA%\BeDD\servers\s1
BeDD.exe server start s1 --json             # pid, port, portV6, url, token, dir, log
BeDD.exe mod config 1.26.60.05-1 --world-mode ClientServer
BeDD.exe launch --slot 1 --json             # then: join 127.0.0.1:<port>
BeDD.exe server stop s1                     # sends stop (saves); kills after 30 s
```

The server's WebSocket (`kind: "server"` in `ps --json`) takes `ping`, `state`, `server`, `timescale`, `command`
([Server target](/reference/websocket#server-target)), e.g. `{op:"command", command:"time set day"}`.

### Setting up a server

`server init` installs LeviLamina's server package with lip (downloaded to `tools`; brings BDS 1.26.60, ~100 MB) and BeDD's
server mod (`mods\BeDD-server`). Idempotent, one at a time per PC, never prompts.

| Flag | Default / meaning |
|---|---|
| `<name>` | `server`; a folder under `servers` (`BEDD_SERVERS_DIR`) |
| `--port`, `--port-v6` | earlier ports, else the first free pair from 19140/19141 up (never 19132) |
| `--transport` | `nethernet` (`raknet` fails: "Terracotta"): TCP `server-port` signaling, UDP `127.0.0.1:<port>` (`server-udp-ports`). First join asks "Trust this server?" |
| `--server-ip` | `127.0.0.1` (only this PC joins); `''`: all interfaces |
| `--online-mode` | off; on: Xbox sign-in required |
| `--property key=value` | any `server.properties` key, repeatable; BeDD defaults `gamemode=creative`, `difficulty=peaceful`, `allow-cheats=true` |
| `--set key=value` | a server mod config key, repeatable: `timeScale`, `trace`, … |

`server list` shows folders, ports and state; `server remove <name>` deletes one. For parallel runs give each
server its own name (e.g. its game's slot) and omit `--port`.

### Starting

`server start <name>` runs `bedrock_server_mod.exe` windowless (output: `logs\console.log`) and waits for the WebSocket
(`--timeout`, 180 s). Against firewall prompts it refuses a server whose `mods\BeDD-server` lacks BeDD's mod, holds
another, or sets `"loopbackOnly": false`, and stops it if the mod fails to load; `server init` sets
`server-ip=127.0.0.1`, LAN visibility off. `--allow-firewall-prompt` overrides (other PCs: `--server-ip ''`).

## Remote

```powershell
BeDD.exe mod config 1.26.60.05-1 --world-mode Remote --server-host play.example.net
BeDD.exe launch --slot 1 --exclusive-signaling --json
```

Only `allowHosts` is reachable off loopback ([No firewall prompts](/guide/configuration#no-firewall-prompts)). Online
sign-in servers need `--exclusive-signaling` ([rules](/guide/several-games#rules-for-parallel-runs)).
