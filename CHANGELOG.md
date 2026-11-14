# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

This will be **0.4.0**, a breaking release: Be.D.D. becomes one app, `BeDD.exe`, and the game speaks WebSocket only. The PowerShell client, the named pipes and the setup scripts are gone.

### Breaking

- **The transport is a WebSocket, and the only one.** The mod serves `ws://127.0.0.1:<port>/v1` instead of `\\.\pipe\<pipeName>`, `\\.\pipe\<pipeName>-tap` and the manager pipe. A client needs the token from the game's discovery file. Requests, replies and op names are the same JSON as before, one message each instead of one line each.
- **No PowerShell left.** `BeDD.psm1`, `harness-shim.ps1`, `Start-BeDDGame.ps1`, `BeDDManager.ps1`, `BeDDSetup.psm1`, `Initialize-BeDD.ps1`, `clone-instance.ps1`, `Start-BeDDServer.ps1`, `tools/build.ps1`, `tools/dev.ps1` and the showcase scripts are removed; `BeDD.exe` and Node scripts replace them.
- **Clones live under `%LOCALAPPDATA%\BeDD\instances\<gameVersion>-<slot>`**, slot 1 included, and the LeviLauncher instance is never written to. Clones next to the instance (`versions\<base>-hi<N>`) are no longer made or used.
- **Release asset `bedd-windows-x64.zip`** (`BeDD/BeDD.exe`, `BeDD/mod/BeDD/`, `BeDD/mod/BeDD-server/`, `BeDD/spec/`) replaces `bedd-client-windows-x64.zip` and `bedd-server-windows-x64.zip`.
- **Needs LeviLauncher 1.1 or newer**, whose command line BeDD uses to find the instance.
- Config keys `pipeName`, `pipeInstances` and `managerScript` are gone (ignored if present); the feature keys `gameHooks`, `tap`, `allowSend` and `allowStack` now default to `true`.

**Migrating from the PowerShell API.** Launch and set up with `BeDD.exe`, then send the same ops over the WebSocket:

| Before | Now |
|---|---|
| `Initialize-BeDD.ps1 -Slot 2`, `clone-instance.ps1 -Slot 2` | `BeDD.exe instance clone --slot 2` (or let `launch` do it) |
| `Start-BeDDGame.ps1 -Slot 2 -Label x` | `BeDD.exe launch --slot 2 --label x --json` (prints `url` and `token`) |
| `Install-BeDDMod` / `Uninstall-BeDDMod` | `BeDD.exe mod install` / `mod uninstall`, `mod config` for the keys |
| `Set-WorldSinglePlayer <dir>` | `BeDD.exe world singleplayer <dir>` |
| `Connect-BeDD -PipeName p` | open `url` with the `token` (`BeDD.exe ps --json`, or `%LOCALAPPDATA%\BeDD\run\<pid>.json`) |
| `Send-BeDDKey E`, `Get-BeDDWorld`, … | `{"id":1,"op":"key","key":"E"}`, `{"id":2,"op":"world"}`, …: the ops the cmdlets sent |
| `Save-BeDDScreenshot` | the `screenshot` op (base64 PNG or JPEG) |
| `BeDDManager.ps1 -List` / `-Shutdown` | `BeDD.exe ps` / `BeDD.exe manager stop` |
| `Initialize-BeDDServer`, `Start-BeDDServer.ps1 [-Stop]` | `BeDD.exe server init`, `server start`, `server stop` |
| `tools/build.ps1`, `tools/dev.ps1` | `xmake` and `dotnet publish` ([building](https://docs.levimc.org/BeDD/reference/building)), `BeDD.exe dev` |
| `Register-BeDDNative` (harness C# input classes) | none: drive the game over the WebSocket from the harness |

The entries below that name PowerShell scripts or pipes describe changes made before the switch; their scripts are replaced as in this table.

### Changed

- **LeviLamina 26.60.7.**
- `gameHooks`, `tap`, `allowSend`, `allowStack` and the new `ui` default to `true`: every mode works out of the box, and the token is the boundary. A key set to `false` still answers `code:"disabled"`.
- The manager window is part of `BeDD.exe` instead of a PowerShell script, and finds games through their discovery files. A game waits 30 s for it (90 s before).
- The showcase recording and encoding scripts are Node scripts.
- **Minecraft 1.26.60 and LeviLamina 26.60** (client 1.26.60.05, BDS 1.26.60.06); 1.26.51 is no longer supported. The setup scripts look for and install those versions, and the tap's `HELLO` reports `game:"1.26.60.05"` and protocol 2225. The game no longer exports several functions BeDD used, so:
  - `slot` and `action` press the keys of the default key map (hotbar `1`..`9`; drop Q, dropAll Ctrl+Q, inventory E, pause Escape) instead of calling `ClientInputCallbacks::handle*`; `perspective` still calls its callback. `slot` replies `via` keys or none (already held), and `action pause` is refused with `busy` while a screen is open, like the other actions (Escape would close the screen).
  - A failed `join` no longer replies `reason`.
  - The game joins servers over NetherNet only (a RakNet server fails with "Terracotta"), so `Initialize-BeDDServer -Transport` defaults to `nethernet`, and a server kept on loopback publishes `127.0.0.1:<port>` as its UDP address (`server-udp-ports`). With `loopbackOnly`, the game now also binds its sockets for an interface's own address to loopback (NetherNet binds one per interface; not with `allowHosts`), and a dual-stack socket goes to `::ffff:127.0.0.1` rather than `::1`, which reached no IPv4 server. Joins to a BDS on the PC work again.
  - `join` answers the two new prompts in its way, with the keys a player would press: "Online play is not rated" (Proceed, then it connects again) and "Trust this server?" for the server it joins (Trust and Join).
  - A server's mods are in `mods\` with LeviLamina 26.60 (`plugins\` before): `Initialize-BeDDServer` installs the BeDD server module to `mods\BeDD-server`, and `Get-BeDDServer` looks there.
  - The tap follows the client's server link through its NetworkSystem's open connection, takes clientbound packets from the connection's batched peer, and finds `LoopbackPacketSender::sendTo` and `sendBroadcast(except)` in the sender's vtable.
  - `timescale` (client and server) runs its server-thread work from a `Minecraft::update` hook instead of `ServerInstance::queueForServerThread`.
- Be.D.D. is a standalone testing toolkit, no longer a mod package: `tooth.json` is gone, so it is neither a lip package nor listed on LeviMC Mods. Install it from the release zip. It has its own documentation at <https://docs.levimc.org/BeDD/> (`docs/`).
- **A BeDD game runs one way only: headless, inside the manager window.** There are no input modes any more (the debug modes under Added are about where the world runs, not about how the game is shown). Loading the mod always shields the game from the real mouse and keyboard and keeps its window a tool window off screen (no taskbar button, not in Alt+Tab, never on screen); the manager window is the only place a game shows up. A game that finds no manager window starts one (new config key `managerScript`, the path of `BeDDManager.ps1`, which `Install-BeDDMod` writes), and a game that no manager links within 90 s, or that loses the link for good, closes itself (logged). Without `config.json` the mod still runs headless, with the defaults.
- `mute` now also gives each FMOD system the NOSOUND output before it initialises: a muted game opens no audio device, so it no longer shows up in the Windows volume mixer.
- `Start-BeDDGame.ps1` refuses an instance without `mods\BeDD\BeDD.dll` and throws when the manager window does not start, and so does the harness shim's `Start-Game` when the manager window does not start. `tools/dev.ps1` starts the manager window and the game shows up there. `tools/showcase/record.ps1` records the games in a manager window of its own (`BEDD_MANAGER_PIPE`), laid out next to the terminal.

### Added

- **The rest of headless bedboo in bedd mode**: `action swing` (`hand` main|off) swings the arm with no attack (the client sends animate itself; `bot.swingArm`); the new `settings` op (`renderDistance` in chunks, config `gameHooks`, listed in `ping.modes.bedboo`) sets the client's render distance for every renderer and has the client request that chunk radius (`bot.setSettings({ viewDistance })`); `join {host: null}` leaves the world back to the menus (`bot.quit()`); `sendAllow` now defaults to `[9, 77, 78, 97]`, adding command_block_update and book_edit, which the client sends from its own command block and book screens (`bot.setCommandBlock`, `bot.writeBook`, `bot.signBook`).
- **Windows installers** (WiX Toolset 7): `bedd-windows-x64-setup.exe` and the MSI inside it, `bedd-windows-x64.msi`. Per user, no administrator rights: `%LOCALAPPDATA%\Programs\BeDD`, `BeDD.exe` on the user's `PATH`, **BeDD** in the Start menu (the manager window). An upgrade closes the manager window through the Restart Manager instead of waiting for a reboot; uninstalling keeps `%LOCALAPPDATA%\BeDD`. `bedd-windows-x64.zip` stays for use without an installer.
- **`launch` keeps a clone's mod current**: it installs the mod that comes with `BeDD.exe` into a clone that has none, and replaces the clone's mod when it differs (after an upgrade, or a new build in a checkout), keeping its `config.json`. `--keep-mod` starts the clone with the mod it has.
- **No console flash**: opened from Explorer or the Start menu on Windows 11 24H2 or later, `BeDD.exe` gets no console window (its manifest asks for `consoleAllocationPolicy` `detached`); from a terminal it still is a command line.
- **`BeDD.exe`**, one NativeAOT app (C#, .NET 10, no runtime to install). Double-clicked, it is the manager window; with arguments, a command line in LeviLauncher's style, `BeDD.exe <noun> <verb> [--json]`: `instance list|show|clone|refresh|remove|path`, `mod install|uninstall|config`, `world singleplayer`, `launch`, `ps`, `stop`, `server init|start|stop|list|remove`, `manager status|stop`, `dev` and `version`. stdout carries only the result, `--json` prints one JSON document, and errors come with an exit code and a runnable hint.
- **WebSocket transport.** 127.0.0.1 only, port from config `wsPort` (default 0: any free port), path `/v1`; a random token per start (`Authorization: Bearer` or `?token=`); requests with an `Origin` header refused unless config `allowOrigins` lists it; up to `wsMaxClients` connections (default 8); a connection that goes releases the keys and mouse buttons it holds and its open-ended `button` holds (`click`, `holdTicks` and `releaseOn` holds, and the `input` override, run to their end). The mod announces itself in `%LOCALAPPDATA%\BeDD\run\<pid>.json` (`url`, `token`, `instance`, `label`, …; `BEDD_HOME` moves the folder). Specified in `spec/bedd.asyncapi.yaml` (AsyncAPI 3.0), documented on the [WebSocket reference](https://docs.levimc.org/BeDD/reference/websocket).
- **Three operation modes, all on at once.** `ping` reports `protocol: 2`, `defaultMode: "bedboo"` and the ops of each mode: `bedboo` (semantic ops, the default for scripts and agents), `ui` and `input` (simulated keyboard and mouse, the last resort).
- **UI tree** (`ui` op, config `ui`; [#21](https://github.com/LiteLDev/BeDD/issues/21)), built for agents that pay per token: the screen in front as one line per control (`- button "Play" [e1]`), a few hundred characters per screen (the title screen: 519 instead of 20,569), alike lines folded (`x153 [in e49]`), capped at `maxNodes` lines. Look closer on demand: `find` (matching lines), `ref` (one control's subtree), `ref` + `detail` (every field), `format:"json"` (the whole tree as objects, for programs). JSON UI and Ore UI screens alike: an Ore UI page's DOM is mapped to the same roles. `click`, `setText` and `scroll` on a `ref`, and `mouse` with a `ref`; a ref names one control while its screen lives, across reads and connections (no snapshot ids). An action answers `changed`, the target's line, the screen in front, and that screen's view when another one came to the front. Codes `stale_ref` (with look-alike `candidates`), `occluded`, `not_ready`, `unsupported`. The `oreui` op gives the raw DOM of the Ore UI view.
- **`screenshot` op**: the game's picture as base64 PNG or JPEG, scaled to `maxWidth` (960 by default), optionally a region, while the window is covered.
- **`tap subscribe`**: the tap is a stream of binary WebSocket messages on the game's socket, one frame each, same framing and markers as before.
- **`manager` and `close` ops**: the manager window attaches to each game over its WebSocket. Config `manager` (default `true`): a game that no manager attaches within 30 s, or that loses it, closes itself; `false` keeps the game hidden and running without one (CI).
- Hardlinked clones per slot under `%LOCALAPPDATA%\BeDD\instances` (copies across volumes), refreshed when the LeviLauncher instance changes, found through `LeviLauncher.exe instance list --json` (`BEDD_LEVILAUNCHER`). Games start without taking the focus (`CreateProcessW`, `SW_SHOWNOACTIVATE`).
- bedboo's `transport: 'bedd'` connects over the WebSocket.
- **Server target** for the client+server debug mode: `--target_type=server` (`tools/build.ps1 -TargetType server`) builds `bin\BeDD-server\`, a mod for a Bedrock Dedicated Server with LeviLamina (release asset `bedd-server-windows-x64.zip`; the client zip is unchanged). It serves `\\.\pipe\BeDD-Server` (config `pipeName`) with `ping`, `state`, `server` (tick, measured TPS, players, port), `timescale` (probe; `scale` 0.05..20 and/or `paused`, run on the server thread and broadcast to every player as LevelEvent 3007, 0 while paused, and to each player who joins; BeDD on the client follows the speed and the pause) and `command` (runs a command as the console, returns its output). Config `timeScale` sets the speed once the server has started. Its `loopbackOnly` (default on) keeps the server on loopback, IPv4 and IPv6 ports, LAN discovery and NetherNet included, so it raises no Windows Firewall prompt.
- **Debug modes**, about where the world runs (config `mode`; `Start-BeDDGame.ps1 -Mode`, `Install-BeDDMod -Mode`, `Set-BeDDMode`; reported by `state` and `join`): `ClientOnly` (default, a local world, as before), `ClientServer` (a dedicated server with BeDD on this PC, joined over loopback) and `Remote` (a server without BeDD at `-ServerHost`, possibly off the PC). Remote mode writes the server into the new config key `allowHosts`: `loopbackOnly` then lets the game query that host and reach it over UDP from an implicitly bound socket (no firewall prompt), keeps listeners on loopback and refuses datagrams to any other host off the PC (`state` `stats.sendsRefused`). Without `allowHosts` the network hooks behave as before.
- **`join` op** (`Join-BeDDServer -Host -Port [-Wait] [-TimeoutMs]`): the client joins a server by address through the game's own external-server join, from the title screen or from inside a world (left first); it replies once the connection started, or with `wait` once the client is in the level, failed (`join_failed`) or timed out. Without `host` it reports the current connection (none, local world, remote host:port).
- `BEDD_MANAGER_PIPE` (or `BeDDManager.ps1 -PipeName`) names another manager window than the session's, for scripts and games alike, e.g. one of its own for a recording.
- Setup for a new PC: `Initialize-BeDD.ps1` and `BeDDSetup.psm1` (`Initialize-BeDDInstance`, `Get-BeDDInstance`, …) find the LeviLauncher instance of Minecraft 1.26.51 and set up whatever is missing: LeviLauncher (downloaded from its releases when not installed), LeviLamina (installed with lip, which is downloaded with its .NET runtime), LeviLauncher's loader in the game exe, and clones. Only the game install itself is left to a person: LeviLauncher opens and the script waits for it. `Start-BeDDGame.ps1`, `clone-instance.ps1` and `tools/dev.ps1` run the same setup, and the harness shim exports it. Overrides: `BEDD_INSTANCE`, `BEDD_VERSIONS_DIR`, `BEDD_LAUNCHER_DATA`, `BEDD_LEVILAUNCHER`, `BEDD_LIP`.
- `Start-BeDDGame.ps1 -Slot N`; `-Instance` is optional (the instance BeDD finds).
- `tools/build.ps1`: builds with xmake, installing xmake, LLVM and the Visual Studio C++ build tools with winget when missing.
- Ops for [bedboo](https://github.com/LiteLDev/bedboo)'s transport `bedd`, which drives this client with bedboo's mineflayer API. Each is off by default and answers `code:"disabled"` (naming its config key) while off; CONTRIBUTING.md has the protocol and the table of which packets the client owns:
  - **Tap** (config `tap`): `\\.\pipe\<pipeName>-tap` streams every game packet the client receives and an echo of the ones it sends, with `HELLO`/`LINK_UP`/`LINK_DOWN`/replay markers, on a remote server and (config `tapLoopback`, default on with `tap`) in singleplayer and LAN-host worlds, where packets are re-serialized and LAN guests' traffic is left out. A join journal (`tapJournalMiB`, default 128) replays a compacted copy of the session since `start_game` to a reader that attaches late, every sub-chunk included. While the tap is on the client asks servers not to use its blob cache, so every sub-chunk arrives with its data. The `tap` op and `state` (`hooks`, `stats.journal`, `stats.tapLoopback`) report it.
  - **Game ops** (config `gameHooks`), run on the client thread: `world` (screen, mouse grab, menu, the player's pose), `look` (absolute yaw/pitch), `input` (walk, jump, sneak, sprint through `ClientInputUpdateSystem`, whatever the key bindings, with a dead-man TTL), `slot`, `action` (drop, drop all, inventory, perspective, pause) and `button` (attack/use/pick released by game ticks or when the targeted block breaks). BeDD.psm1: `Get-BeDDWorld`, `Set-BeDDLook`, `Set-BeDDInput`, `Set-BeDDSlot`, `Invoke-BeDDAction`, `Set-BeDDButton`.
  - **`send`** (config `allowSend`, ids in `sendAllow`, default text and command_request): the local player sends a packet a script serialized; login, inventory_transaction, player_auth_input and item_stack_request are always refused. `Send-BeDDPacket`.
  - **`stack`** (config `allowStack`): the local player sends an item_stack_request a script serialized, with its request ids renamed to client ids (the reply maps them back), and asks the server for the whole inventory once a request is accepted so the game shows the result. `view:true` reports what the client shows. `Send-BeDDStack`.
- **Game speed** (`timescale` op, `Get-BeDDTimeScale`, `Set-BeDDTimeScale [-Scale] [-Paused]`, config `timeScale`): a world this game hosts runs at 0.05..20 times normal speed, the client and its local server together, or pauses; the reply comes once both sides apply it and reports the ticks per second measured on each (a local world reaches 400 at 20). Entering a world applies `timeScale` (default 1) and leaving one resets the client. On a remote server a change answers `code:"remote"`; on a server running BeDD the server leads (it sends level event 3007 SimTimeScale, which the client applies) and a change answers `code:"server_led"`. `Install-BeDDMod -TimeScale` and `tools/dev.ps1 -TimeScale` write the key.
- Several scripts can use a game's control pipe at once (config `pipeInstances`, default 4; `1` = one at a time as before). Window input is serialized between them, `state` reports `clients`, and a client that disconnects (or dies) has what it still holds released: its keys and mouse buttons, its `button` holds and its `input` override, and nothing another client holds. Mod code can tag per-client state with `currentPipeClient()`.
- Ops follow the game speed: "in a world" means a client tick within 20 measured tick intervals (at least 1 s, as before at 20 TPS), and `slot`, `action`, `button`, `look`, `send` and `stack` without `timeoutMs` wait longer by as much as the client ticks slower than 20 a second (explicit `timeoutMs` values stand). `input` can take `ttlTicks` instead of `ttlMs`: the override lasts that many client ticks at any speed (probe: `ticksLeft`); `Set-BeDDInput -TtlTicks`. `Set-BeDDLook`, `Send-BeDDPacket` and `Send-BeDDStack` send `timeoutMs` only when `-TimeoutMs` is given.
- Client + server mode, setup side: `Initialize-BeDDServer` (`BeDDSetup.psm1`) sets up a Bedrock Dedicated Server 1.26.51 with LeviLamina in `%LOCALAPPDATA%\BeDD\servers\<name>` (`BEDD_SERVERS_DIR`): lip installs LeviLamina's server package into an empty folder, which brings the official BDS with it. It writes `server.properties` from parameters (ports default to the first free pair from 19140/19141, offline mode, creative, cheats on) and installs the BeDD server module from `-ModuleDir` into `plugins\BeDD-server` with its `config.json` (`pipeName` default `BeDD-Server-<name>`, `timeScale`, `trace`). Idempotent, no prompts. `Start-BeDDServer.ps1` runs it with no window, its console in `logs\console.log`, waits for it and for BeDD's pipe, and returns `Process`, `PipeName`, `Port`, `Dir`, `LogPath`; `-Command` types into its console, `-Stop` stops it (`stop`, killed after a timeout). It refuses to start a server without the BeDD server module or with its `loopbackOnly` off (the module binds the server to 127.0.0.1, so Windows Firewall never asks on the desktop) unless `-AllowFirewallPrompt`; servers use the `raknet` transport, which `Join-BeDDServer` joins (BDS 1.26.51 logs that players need NetherNet; they do not), and listen on 127.0.0.1 with no LAN visibility, and a server whose module LeviLamina fails to load is stopped before it opens its network. `Get-BeDDServer` reports one. `Connect-BeDD` accepts a server's ping (no window). Docs: [Client and server modes](https://docs.levimc.org/BeDD/guide/modes).

### Removed

- The PowerShell client and every script (see *Breaking*), the control pipe, the tap pipe and the manager pipe, `BEDD_MANAGER_PIPE`, and the config keys `pipeName`, `pipeInstances` and `managerScript`.
- The `bedd-client-windows-x64.zip` and `bedd-server-windows-x64.zip` release assets.
- Background mode as a switch, and every way to run a BeDD game with its own window: config `background`, `placement` and `manager` (0.4.0 brings back a `manager` key with another meaning, under *Added*); `Start-BeDDGame.ps1 -NoManager` and `BEDD_MANAGER`; `Install-BeDDMod -Placement` and `-NoManager`; `Set-BeDDBackground` and the pipe op `background`; `Set-BeDDWindow -Placement` and the `placement` field of the `window` op; Ctrl+Shift+F12; `tools/dev.ps1 -Placement` and `-NoBackground`.
- Taking a game over from the manager window: the "take over with my mouse and keyboard" and "hand back" menu items, `BeDDManager.ps1 -Game <pid> -Action show|hide` (only `close` is left) and the manager link ops `show` and `hide`. `ping`/`state` no longer report `background` and `takenOver`, nor the manager's `hello` and `list`.

### Fixed

- A one-shot caller (send `button click`, close the connection) got `ok:true` and nothing happened: the closing connection released the button before the game saw it held. A `click`, a `holdTicks` hold and a `releaseOn` hold now run to their end (a `releaseOn` hold at most 1200 ticks after its connection closed), and the `input` override to its `ttlMs`/`ttlTicks`.
- `launch` from a checkout replaced a mod installed with `mod install --from <dir>` by the checkout's `bin\BeDD` without a word. `--from` now pins the instance to that folder (`mods\BeDD\source.pin`): `launch` keeps the mod current with it, and warns and leaves the mod alone when the folder has no build any more; an install without `--from` ends the pin.
- **A BeDD game is always silent**: config `mute` is ignored (`false` only logs a warning). A headless game is heard by nobody on purpose, and harnesses that wrote `"mute": false` played every game's sounds on the user's speakers.
- A muted game could open an audio device again when it switched FMOD's output after initialising it (an audio device change, say): every `System::setOutput` of a muted game now gets NOSOUND.
- Nothing worked on a PC that differs from the author's: the scripts expected LeviLauncher at `Desktop\LeviLauncher.exe`, its instances in `%APPDATA%\levilauncher.exe\versions` (LeviLauncher's data folder setting was ignored), an instance named `1.26.51.01`, and ffmpeg in one user's winget folder. An instance never started from LeviLauncher also silently ran without its data isolation, since only LeviLauncher adds its loader to the game exe.
- An existing clone kept stale game files after its base instance was updated (`clone-instance.ps1` now refreshes them).
- `tools/dev.ps1` closed every Minecraft on the PC, including other runs' games; it only closes its own instance's now.

## [0.3.1] - 2026-10-30

### Changed

- The package is listed as Be.D.D. (`tooth.json` `info.name`), on LeviMC Mods too.

## [0.3.0] - 2026-10-30

### Changed

- **Renamed to Be.D.D. (Bedrock Deep Dive), after the H.D.D. of Zenless Zone Zero.** The repository is `LiteLDev/BeDD` and the lip package `github.com/LiteLDev/BeDD`; everything else follows, with no aliases for the old names:
  - mod folder and DLL `mods\BeDD\BeDD.dll`, build output `bin\BeDD\`, release asset `bedd-client-windows-x64.zip`;
  - control pipe `\\.\pipe\BeDD` (config `pipeName` default) and manager pipe `\\.\pipe\BeDD-Manager-<session id>`;
  - client `BeDD.psm1` with `Connect-BeDD`, `Disconnect-BeDD`, `Test-BeDD`, `Invoke-BeDD`, `Clear-BeDDInput` and `*-BeDD*` for every other `*-Headless*` cmdlet (`Send-BeDDKey`, `Get-BeDDState`, …); `Start-BeDDGame.ps1`, `BeDDManager.ps1`;
  - harness shim `Install-BeDDMod`, `Uninstall-BeDDMod`, `Register-BeDDNative`;
  - environment variables `BEDD_LABEL`, `BEDD_MANAGER`, `BEDD_PIPE`, `BEDD_RETURN_FOCUS`; harnesses use `BEDD_DIR` in place of `LEVI_HEADLESS_INPUT_DIR`.

### Added

- Config `mute` (default `true`): in background mode the game is silent. The mod hides focus loss from the game, so it never paused its audio, and headless games played menu music and sounds on the user's speakers. Taking a game over makes it audible again; `state` reports `audio`.

### Fixed

- A console window flashed on the desktop at every game start: LeviLamina starts its crash logger, `CrashLogger.exe`, as a console program, and the game has no console to give it. `Install-BeDDMod` and `Start-BeDDGame.ps1` (for background games) switch the instance's LeviLamina to its built-in crash logger (`modules.crashLogger.builtin`).

## [0.2.0] - 2026-10-29

### Added

- The manager window (`HeadlessManager.ps1`): one per logon session, started by the first `Start-HeadlessGame.ps1` (or the shim's `Start-Game`) and reused by every later run. Every headless game shows up there as a live picture instead of as a window of its own (off screen, no taskbar button, not in Alt+Tab), labelled with the project and worktree that started it. Closing the manager closes all its games (also when it is killed); right-click a game to take it over with the real mouse and keyboard, hand it back, open its log or close it. `-List`, `-Game <pid> -Action show|hide|close` and `-Shutdown` do the same from scripts. Config `manager` (default `true`), `Start-HeadlessGame.ps1 -Label/-NoManager`, `Install-HeadlessInputMod -NoManager`, `LEVI_HEADLESS_MANAGER=0`.
- `ping` and `state` report `managed` and `takenOver`.
- `state` reports `serverQueriesRefused`, and GameInput statistics report `virtualReadings`.

### Fixed

- A game could take the foreground for a moment when it started (with background mode on): the "focus gained" messages the mod sends activated the window when the game had been started from the user's foreground app (a terminal, an agent). They no longer activate it, and the user32 hooks are in place before the game creates its window.
- `enable()` could still hang for good while installing the hooks, most often with several games starting at once: a thread stopped by the hook pause while holding the process heap lock blocked the hooking code. The heap stays locked while the threads are paused now.
- Virtual clicks, wheel and camera were lost while nobody touched the real mouse (unattended runs stayed on the title screen with Play highlighted): GameInput has no mouse reading until the device reports input. The game now gets a virtual reading of the device instead.
- With `loopbackOnly`, a game could join a "LAN world" instead of the test world and hang at "Connecting to multiplayer game" (~97%): game servers queried through a proxy on the PC were listed as LAN worlds ahead of the test world. Game servers outside the PC are no longer queried.
- With `loopbackOnly`, outgoing TCP connections (WinHTTP) were bound to loopback and reached the internet only through a proxy on the PC. They are left alone now.
- `loopbackOnly` stays in force after the game is handed back (`Set-HeadlessBackground $false`, Ctrl+Shift+F12): sockets opened afterwards raised the firewall prompt.

## [0.1.3] - 2026-10-24

### Added

- A logo, and a recording of three games driven at once in the background, in the [LeviMC Mods gallery](https://levimc.org/mods/github.com/LiteLDev/BeDD/?tab=gallery).

### Changed

- The README is rewritten for people writing test scripts. How it works inside, the pipe protocol and build notes moved to [CONTRIBUTING.md](CONTRIBUTING.md).

## [0.1.2] - 2026-10-23

### Added

- Loopback-only sockets (`loopbackOnly`, on by default): in background mode `bind()` to 0.0.0.0/:: is turned into a loopback bind, so a new game instance (a new exe path) no longer raises the Windows Firewall "allow access" prompt that stopped unattended runs. Outgoing connections (local servers, the internet) are unaffected; nothing outside the PC can reach a test game. `state` reports `loopbackRebinds`.

- Safety net in the `Start-Game` wrapper: worlds of the instance written in the last 15 minutes (the test world a harness just restored) are made single-player before the game starts, so a harness that forgot `Set-WorldSinglePlayer` no longer breaks other projects' parallel runs with "Blaze". Older worlds are never touched.

## [0.1.1] - 2026-10-23

### Added

- Exclusive signaling lock also waits until no `Minecraft.Windows` process is left, so games started outside the lock cannot break online-multiplayer runs.
- Machine-wide signaling lock in `harness-shim.ps1`: every headless game holds it shared from `Start-Game` to `Stop-Game`; runs that join a server over NetherNet set `$script:ExclusiveSignaling = $true` and get the machine to themselves (each game's sign-in to the signaling service knocks out the previous one).

### Fixed

- Virtual mouse clicks, wheel notches and camera moves arrived once per mouse device (twice on a PC with two mice): the virtual state now goes to one device and the others report idle.
- `enable()` could hang while installing the hooks: all hooks of a batch are now installed during a single thread pause, and nothing is logged while threads are paused.

## [0.1.0] - 2026-10-22

### Added

- Control pipe (`\.\pipe\<pipeName>`, JSON lines) with `ping`, `state`, `key`, `text`, `mouse`, `raw`, `release`, `focus`, `window`, `background` and `flush`; input ops reply once the game thread has processed the input.
- Keyboard, text and pointer position dispatched through the game's own window procedure (subclassed window).
- Virtual GDK GameInput mouse (v2 `IGameInputReading` hooks): buttons, wheel and camera in menus and in the world; the real mouse and keyboard no longer reach a background game.
- Background mode: focus-loss messages swallowed (no pause), real client-area input swallowed, `ClipCursor`/`SetCursorPos`/`GetCursorPos`/`GetAsyncKeyState`/`GetKeyState`/`GetKeyboardState`/`GetForegroundWindow`/... report a virtual state, window activation hooks keep the game out of the foreground. Ctrl+Shift+F12 on the real keyboard turns it off.
- PowerShell client `HeadlessInput.psm1` (input, screenshots via `PrintWindow`, per-instance pipes).
- `harness-shim.ps1`: drop-in replacement of the usual LeviLauncher harness helpers, plus redirects for harness C# input classes and `Set-WorldSinglePlayer` (parallel instances share one signaling-service ID and knock each other out of multiplayer worlds: "An error has occurred", code "Blaze").
- `Start-HeadlessGame.ps1` (start an instance without activating it) and `clone-instance.ps1` (cheap clone instances for parallel games).
- Trace mode: message, API-hook and GameInput statistics, and every input event that reaches the game, marked synthetic or real.

[Unreleased]: https://github.com/LiteLDev/BeDD/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/LiteLDev/BeDD/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/LiteLDev/BeDD/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/LiteLDev/BeDD/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/LiteLDev/BeDD/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/LiteLDev/BeDD/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/LiteLDev/BeDD/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/LiteLDev/BeDD/releases/tag/v0.1.0
