# Configuration

The mod reads `mods\BeDD\config\config.json` (missing: defaults). Edit only via `BeDD.exe` (hardlinked files):

```powershell
BeDD.exe mod config 1.26.60.05-2                                        # print
BeDD.exe mod config 1.26.60.05-2 --set clientWidth=1600 --unset timeScale # --unset: back to default
```

`--set` (also on `mod install`): JSON if it parses, else a string. `--world-mode` sets `mode` and
`allowHosts` ([Where the world runs](/guide/modes)). `mod install` and `launch` also set
`modules.crashLogger.builtin` in `mods\LeviLamina\config\Config.json` (else `CrashLogger.exe` flashes a console).

## Keys

| Key | Default | |
|---|---|---|
| `clientWidth`, `clientHeight` | `0` (leave) | window size, for stable click positions |
| `wsPort` | `0` (free port) | WebSocket port on `127.0.0.1`, see `run\<pid>.json`; fixed only for one game at a time |
| `wsMaxClients` | `8` | connections at once |
| `allowOrigins` | `[]` | web origins allowed to connect; other `Origin` headers get 403 |
| `manager` | `true` | show in the [manager window](/guide/several-games#the-manager-window), close without one for 30 s; `false`: hidden, never self-closes (headless CI) |
| `loopbackOnly` | `true` | keep every socket on loopback: no firewall prompt, no "LAN worlds" from other hosts. Server mod: same for its IPv4/IPv6 ports, LAN discovery, NetherNet; `state` reports it |
| `mode` | `ClientOnly` | `ClientOnly`, `ClientServer`, `Remote` ([world modes](/guide/modes)); `state` and `join` report it |
| `allowHosts` | `[]` | off-PC hosts still reachable (the Remote server); while it names one, datagrams to other off-PC hosts are refused (`state` `stats.sendsRefused`) |
| `mute` | ignored | always silent; `false` only logs a warning |
| `trace` | `false` | log every input to `logs\latest.log` |
| `timeScale` | `1` | game speed 0.05..20 of hosted worlds (`timescale` op changes it). Behind a BeDD server the server leads; without BeDD no effect. Server mod: applied at start, sent to players |
| `gameHooks` | `true` | bedboo ops `world`, `look`, `input`, `slot`, `action`, `button` |
| `tap` | `true` | `tap` op ([Tap protocol](/reference/tap)): the player's whole session; disables the blob cache |
| `tapLoopback` | `true` | with `tap`: also singleplayer and hosted LAN worlds |
| `tapJournalMiB` | `128` | join journal cap; past it late attaches get `complete:false` |
| `allowSend` | `true` | `send` op (chat, commands) |
| `sendAllow` | `[9, 77, 78, 97]` | packet ids `send` may send (text, command_request, command_block_update, book_edit); login, inventory_transaction, player_auth_input, item_stack_request always refused |
| `allowStack` | `true` | `stack` op (item stack requests) |
| `ui` | `true` | `ui` op ([UI tree](/reference/ui)) |
| `uiMaxNodes` | `1000` | default node cap of a `format:"json"` `ui` reply (a request's `maxNodes` replaces it) |

## Feature keys

Feature keys `gameHooks`, `tap`, `allowSend`, `allowStack`, `ui`: all on (only your user reads the token). Off: their ops answer `code:"disabled"`;
e.g. `allowSend=false` forces chat through the chat screen. Risks: [Security](/reference/websocket#security).

## No firewall prompts

Windows prompts once per exe path (per clone) on a non-loopback bind. `loopbackOnly` moves wildcard binds to loopback;
with `allowHosts` the outgoing UDP socket connects to the host instead (no prompt). Never turn `loopbackOnly` off for
a remote server.

## Environment variables

| Variable | Read by | |
|---|---|---|
| `BEDD_HOME`, `BEDD_INSTANCES_DIR`, `BEDD_SERVERS_DIR` | `BeDD.exe` (+ mod: `BEDD_HOME`) | [Folders](/guide/new-pc#folders) |
| `BEDD_LEVILAUNCHER`, `BEDD_INSTANCE` | `BeDD.exe` | [Which instance](/guide/new-pc#which-instance) |
| `BEDD_LABEL` | `BeDD.exe`, mod | manager window name (`launch --label`) |
| `BEDD_RETURN_FOCUS` | mod | gets the foreground back if the game takes it at start (set by `launch`) |
| `BEDD_DIR` | your harness | an unpacked Be.D.D. ([Installation](/guide/installation#let-the-harness-fetch-it)) |
