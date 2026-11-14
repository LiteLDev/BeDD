# Contributing to Be.D.D.

Thanks for helping! Bug reports, ideas and pull requests are all welcome.

## Reporting bugs and requesting features

Use the [issue templates](https://github.com/LiteLDev/BeDD/issues/new/choose). For bugs, include:

- the Be.D.D., LeviLamina and Minecraft versions;
- what the script sent (the JSON request, or the `BeDD.exe` command line) and what the game did instead;
- `mods/BeDD/config/config.json`;
- the relevant part of `logs/latest.log`, ideally with `"trace": true` (it shows which input reached the game and whether it was synthetic or real).

## Development setup

See [Development](#development) below. Be.D.D. has two parts: the mod (C++ in `src/`, loaded by
LeviLamina inside the game or a Bedrock Dedicated Server) and the app `BeDD.exe` (C# in `app/`),
which sets up, starts and shows the games. They talk only over the mod's WebSocket.

### The mod

| File | |
|---|---|
| `src/BeDD.h` | shared state and declarations of the parts below (a new unit declares its own things in its own header) |
| `src/Mod.cpp` | entry point, config, virtual key/cursor state |
| `src/Window.cpp` | window discovery, subclassing, synthetic event dispatch, focus shield, window kept off screen |
| `src/ApiHooks.cpp` | user32 hooks (cursor, key state, foreground, activation, raw input) |
| `src/GameInputHooks.cpp` | GameInput v2 hooks (virtual mouse, keyboard readings) |
| `src/Audio.cpp` | FMOD hooks: the game is always silent (NOSOUND output, master channel group muted) |
| `src/NetHooks.cpp` | loopback-only network (`loopbackOnly`: no firewall prompt, no queries to servers off the PC; `allowHosts`: the one server Remote mode reaches); in the server target too |
| `src/ServerTimeScale.cpp` | server target only: the `server`, `timescale` and `command` ops, config `timeScale`, LevelEvent 3007 to the players |
| `src/Join.cpp` | `join` op: the client joins a server by address (the game's own external-server join, on the client thread); where the client plays, for `state` |
| `src/WsServer.cpp` | the WebSocket server: Winsock on 127.0.0.1, the RFC 6455 handshake and framing, token and `Origin` checks, one thread per connection |
| `src/Discovery.cpp` | writes `%LOCALAPPDATA%\BeDD\run\<pid>.json` (`BEDD_HOME`) while the server listens and deletes it at exit |
| `src/Pipe.cpp` | the protocol: `handle()` dispatches every op (one `else if` line per op), per-connection ownership of held input; the name is historical |
| `src/Manager.cpp` | the `manager` and `close` ops: attach, the window kept off screen while attached, closing a game no manager attaches |
| `src/Ui*.cpp` | `ui` op (`ui`): the JSON UI tree read from the game's scene stack, refs, snapshots, and click/setText/scroll through the virtual mouse and keys |
| `src/Screenshot.cpp` | `screenshot` op: `PrintWindow(PW_RENDERFULLCONTENT)` of the game window, encoded with WIC |
| `src/Trace.cpp` | trace-mode input logging |
| `src/MoveInput.cpp` | `input` op: move override in the client's `ClientInputUpdateSystem` with a dead-man TTL (`gameHooks`) |
| `src/Send.cpp` | `send` op (`allowSend`): allowlisted serverbound packets, sent by the local player on the client thread |
| `src/GameState.cpp`, `src/Look.cpp` | `world` snapshot and absolute `look` (`gameHooks`) |
| `src/GameActions.cpp` | `slot`, `action` and tick-based `button` ops (`gameHooks`) |
| `src/Stack.cpp` | `stack` op (`allowStack`): item stack requests sent by the local client |
| `src/TimeScale.cpp` | `timescale` op and `timeScale`: game speed of the client and its local server, tick rates, a BeDD server's scale (level event 3007) |
| `src/TapSink.h` | the frame type and sink chain every tap publisher shares (keep it byte-identical) |
| `src/Tap.cpp`, `src/Tap.h` | the tap: framing, markers, writer thread, the `tap` op and its subscriber |
| `src/NetTap.cpp` | tap capture on a remote server link, and the blob cache switch |
| `src/TapLoopback.cpp` | tap capture in singleplayer and LAN-host worlds (re-serialized packets) |
| `src/TapJournal.cpp` | the tap's join journal (compacted replay for a late reader) |
| `src/MemoryOperators.cpp` | routes `new`/`delete` through LeviLamina's allocator (required by LeviLamina) |
When you hook a game or LeviLamina symbol, check the SDK headers under
`%LOCALAPPDATA%\.xmake\packages\l\levilamina\<version>\<hash>\include` first. Only call `MCAPI`,
`MCFOLD` and `MCTAPI` functions.

### The app

`app/BeDD.slnx`: `app/BeDD/BeDD.csproj` (.NET 10, `WinExe`, NativeAOT, Win32 through CsWin32 and
`app/BeDD/NativeMethods.txt`) and its tests `app/BeDD.Tests` (xunit v3). `Program.cs` opens the
manager window when there are no arguments and runs the command line otherwise, attached to the
parent console.

| Folder | |
|---|---|
| `app/BeDD/Cli/` | the command line: `Dispatcher`, `Registry`, `Help`, `Ctx` (output), and one file per command or small group of commands |
| `app/BeDD/Core/` | everything the commands and the window share: `Paths` (the folders under `%LOCALAPPDATA%\BeDD`), `Discovery` (`run\<pid>.json`), `Instances` (LeviLauncher's instances, read-only, and BeDD's clones), the game's WebSocket client, launching, mod install, servers, `BuildInfo` (game, LeviLamina and protocol versions) |
| `app/BeDD/Gui/` | the manager window (`ManagerApp`): DWM thumbnails of every game, attached over each game's WebSocket |

The command line follows LeviLauncher's (`app/BeDD/Cli/Command.cs` has the details):

- `BeDD.exe <noun> <verb> [arguments] [flags]`, or one word for a top-level command (`launch`,
  `ps`, `stop`, `dev`, `version`). Nouns: `instance`, `mod`, `world`, `server`, `manager`.
- Each command registers itself from a `[ModuleInitializer]` in its own file and touches no other
  file.
- stdout carries only the result. `--json` prints one document (an array for lists, an object
  otherwise, camelCase keys, RFC 3339 times, sizes in bytes); `-q` prints identifiers only;
  notices, warnings and `-v` output go to stderr.
- Failures throw `CliError` with an `ExitCode`: 0 ok, 1 failure, 2 usage, 4 not found, 5 conflict,
  10 declined. The dispatcher prints `error: <message>` and `hint: <a runnable command>`, or
  `{"error":{code,message,hint}}` with `--json`. Messages are plain lower-case English without a
  final period; names match exactly, ignoring case, never by prefix.
- JSON must be AOT-safe: `JsonNode`, or a source-generated `JsonSerializerContext` in the file that
  uses it. No reflection.
- BeDD never writes into a LeviLauncher instance. It finds them with
  `LeviLauncher.exe instance list --json` (`BEDD_LEVILAUNCHER` points at the exe) and clones them
  under `%LOCALAPPDATA%\BeDD\instances\<gameVersion>-<slot>` (hardlinks; copies across volumes).
  LeviLauncher's loader in the clone's `Minecraft.Windows.exe` resolves everything relative to the
  exe's folder, so the exe keeps that name and the clone's data stays in
  `<clone>\Minecraft Bedrock`.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep each pull request focused on one change. Describe what it does and how you tested it.
3. Follow the existing style: C++20 and C# with nullable on, the provided `.clang-format` and
   `.editorconfig`, English in code and comments.
4. Test in game: `BeDD.exe dev` (see [Building](#building)), then drive the game over its WebSocket and check that the
   ops work, the game never takes the foreground and real input does not reach it. Run
   `dotnet test app/BeDD.slnx`, and `node --test tests/ws/` against the running game.
5. CI must pass before a pull request is merged.

Commit messages use the imperative mood ("Add wheel tilt", not "Added …"). Update
[CHANGELOG.md](CHANGELOG.md) under *Unreleased* for user-visible changes.

## Development

Material for people working on Be.D.D. itself, or writing a client for its WebSocket. Script
authors find everything they need in the [README](README.md).

### Building

The full recipe is in [docs/reference/building.md](docs/reference/building.md)
([online](https://docs.levimc.org/BeDD/reference/building)). In short:

- **The mod** needs Windows x64, [xmake](https://xmake.io), Visual Studio Build Tools with MSVC
  14.4x or newer, and LLVM (`clang-cl`) on `PATH`:

  ```
  xmake f -P . -y -m release -p windows --target_type=client
  xmake -P . -y        # -> bin\BeDD\ (DLL and manifest)
  ```

  Always pass `-P .` in a git worktree: without it xmake walks up to the main checkout's
  `xmake.lua` and builds that instead. The mod packer reads the version from `git describe`, so the
  repository needs at least one commit.
- **The server target** (client and server mode): `--target_type=server` builds the mod a Bedrock
  Dedicated Server with LeviLamina runs, into `bin\BeDD-server\` (DLL and manifest, platform
  `server`). It holds the WebSocket server, the loopback-only network and the server ops
  (`src/ServerTimeScale.cpp`) and nothing of the client (`xmake.lua` lists its files; the rest of
  `src/` needs the client's LeviLamina headers). `BeDD.exe server init` installs it into the
  server's `mods\BeDD-server\`.
- `xmake-requires.lock` pins the packages of both targets, but xmake rewrites it with the
  configured target's pins only: `git checkout -- xmake-requires.lock` before you configure the
  other target and before you commit (CI does the same). To move a pin, configure each target,
  keep both targets' entries, and commit the union.
- **The app**: `dotnet build app/BeDD.slnx` and `dotnet test app/BeDD.slnx` need only the .NET 10
  SDK. The NativeAOT publish (`dotnet publish app/BeDD/BeDD.csproj -c Release -o <dir>`) also needs
  the Visual Studio C++ tools and `vswhere.exe` on `PATH`
  (`%ProgramFiles(x86)%\Microsoft Visual Studio\Installer`).
- **The installers**: `dotnet build packaging/wix/Bundle/BeDD.Bundle.wixproj -c Release -p:Platform=x64`
  builds the MSI and `setup.exe` from `bin\release\BeDD` (`.github/actions/stage`);
  `node --test tests/installer/installer.test.mjs` installs, upgrades and uninstalls them (CI runs it).
- **The protocol tests**: `node --test tests/ws/` (Node 22 or newer) runs against a game that is
  running, found through its discovery file.


`bin\app\BeDD.exe dev` is the playground: it clones the base instance for slot 9 (`--slot`),
installs `bin\BeDD` into the clone, launches the game in the background and logs every foreground
and cursor-clip change to `tools\out\desktop-watch.log` until Ctrl+C, `tools\out\dev.stop` or the
game's exit (`tools\out\dev.restart` reinstalls the mod and relaunches). A line ending in
`MINECRAFT HAS THE FOREGROUND` means a game took the focus.

### Showcase media

`docs/` is the documentation site (<https://docs.levimc.org/BeDD/>, built with levimc-kit by
`.github/workflows/docs.yml`; `cd docs && npm ci && npm run build`). `docs/public/showcase/` holds
its gallery and the README pictures. `tools/showcase/` (Node) reproduces the desktop video: it
prepares three clones, restores the `tools/showcase/fixtures/Showcase.zip` world, starts the games
in a manager window, enters the world, runs the demo script off screen and without the focus, and
records the manager window and the script's console with ffmpeg's `gfxcapture`
(Windows.Graphics.Capture: each window's own picture, whatever covers it). Nothing is moved in front
of you and nothing takes the focus. A second script puts a take together, the manager on the left
and the terminal on the right, into the MP4, WebP and poster files.

### Releasing

1. Move the *Unreleased* entries of [CHANGELOG.md](CHANGELOG.md) under the new version and date,
   and add its compare link at the bottom.
2. Tag the commit `v<version>` and push the tag. `.github/workflows/release.yml` builds both mod
   targets and publishes `BeDD.exe`, and attaches three assets to the GitHub release: the
   installer `bedd-windows-x64-setup.exe`, the MSI inside it, `bedd-windows-x64.msi` (per user,
   `%LOCALAPPDATA%\Programs\BeDD`, see `packaging/wix`), and `bedd-windows-x64.zip`:

   ```
   BeDD/BeDD.exe
   BeDD/mod/BeDD/          the client module (DLL, manifest)
   BeDD/mod/BeDD-server/   the server module
   BeDD/spec/              bedd.asyncapi.yaml
   ```

   `BeDD.exe` installs the modules from the `mod\` folder next to it, so keep that layout.
3. Check that `BuildInfo.LeviLaminaVersion` (`app/BeDD/Core/BuildInfo.cs`) matches the
   `levilamina` requirement in `xmake.lua` (CI checks it), and that `ping`.`protocol` and the spec's
   version still agree.

### How it works

The full page is [docs/reference/how-it-works.md](docs/reference/how-it-works.md)
([online](https://docs.levimc.org/BeDD/reference/how-it-works)). In short:

```
 test script / agent / bedboo             Minecraft.Windows.exe + BeDD.dll
 ────────────────────────────             ─────────────────────────────────────────────
 {"id":1,"op":"key","key":"E"} ──text frame──▶ ws://127.0.0.1:<port>/v1   (token)
                                          │ queue Event{WM_KEYDOWN 'E'}; PostMessage(hwnd, WM_LHI)
                                          ▼
                                   subclassed window procedure (game thread)
                                     WM_LHI → apply virtual key/cursor state
                                            → CallWindowProc(game's WndProc, WM_KEYDOWN, ...)
                                     real WM_KEY*/WM_MOUSE*/WM_INPUT/WM_POINTER* → swallowed
                                     WM_ACTIVATEAPP(0)/WM_KILLFOCUS/WM_ACTIVATE(inactive) → swallowed
 ◀──{"id":1,"ok":true}── after the game thread dispatched it
```

- The mod serves a WebSocket on 127.0.0.1 and announces it in `%LOCALAPPDATA%\BeDD\run\<pid>.json`
  (url and token).
- bedboo ops hook the game's own systems and act on client ticks; the `ui` op reads the JSON UI
  tree; the input ops go through the game's window procedure (keys, text, pointer) and a virtual
  GameInput mouse (buttons, wheel, camera), and the user32 hooks keep the real mouse and keyboard
  and the foreground away from the game.
- `BeDD.exe` clones the LeviLauncher instance per slot (hardlinks), starts the clone without
  activating it, and the manager window shows every game through a DWM thumbnail and attaches to
  it over its WebSocket; a game no manager attaches closes itself.

### WebSocket protocol

The protocol is specified in [`spec/bedd.asyncapi.yaml`](spec/bedd.asyncapi.yaml) (AsyncAPI 3.0)
and explained on the [WebSocket reference](https://docs.levimc.org/BeDD/reference/websocket); when
you add or change an op, change the spec in the same pull request. The essentials:

- **Finding a game.** While its server listens, the mod writes
  `%LOCALAPPDATA%\BeDD\run\<pid>.json` (`BEDD_HOME` moves the root) and deletes it at exit:
  `{protocol: 2, kind: "client"|"server", pid, url: "ws://127.0.0.1:<port>/v1", token, instance,
  instanceDir, label, modVersion, startedAt}`. A reader ignores (and may delete) a file whose
  process is gone. `BeDD.exe ps --json` lists them.
- **Connecting.** 127.0.0.1 only, port from config `wsPort` (default 0: any free port), path `/v1`.
  The token (32 random bytes as hex) goes in `Authorization: Bearer <token>` or `?token=<token>`;
  without it the handshake gets HTTP 401. A request with an `Origin` header gets 403 unless config
  `allowOrigins` lists that origin, so a web page cannot reach the game. Text, binary, ping/pong,
  close and fragmented messages; no extensions; at most 16 MiB a message.
- **Messages.** Text messages are JSON. A request is `{id, op, ...}`; the reply echoes `id` and is
  `{"ok":true,...}` or `{"ok":false,"error":"...","code":"..."}` (`key` names the config key of a
  `disabled` op). Input ops reply after the game thread has dispatched the input (`timeoutMs`,
  default 5000). Binary messages carry only the tap.
- **Modes.** `ping` reports `protocol: 2`, `defaultMode: "bedboo"` and `modes`, the ops of each mode:
  `bedboo` (`world`, `look`, `input`, `slot`, `action`, `button`, `send`, `stack`, `timescale`,
  `join`, `tap`), `input` (`key`, `text`, `mouse`, `raw`, `release`, `flush`, `focus`, `window`,
  `screenshot`) and `ui` (`ui`). All three are on; a client should prefer bedboo, then the UI tree,
  and use input last.

| op | fields | |
|---|---|---|
| `ping` | | `protocol`, `defaultMode`, `modes`, `pid`, `hwnd`, `client{w,h}`, `cursor{x,y}`, `realForeground`, `managed` |
| `state` | | `ping` fields + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput, loopbackRebinds, serverQueriesRefused, audio, tapLoopback, send, journal, sendsRefused}`, `clients` (connections now), `hooks{name: installed}` (each game hook a feature installed; `false` while the feature is off), `mode` (ClientOnly\|ClientServer\|Remote, config `mode`), `connection{connection none\|local\|remote, inLevel, host, port}` (where the client plays) |
| `key` | `key` (name) or `vk`; `action` press\|down\|up; `holdMs` | names: letters, digits, ESC, ENTER, BACK, TAB, SPACE, SHIFT, CTRL, ALT, LEFT/UP/RIGHT/DOWN, HOME, END, PGUP, PGDN, INSERT, DELETE, F1..F24, SLASH, GRAVE, MINUS, EQUALS |
| `text` | `text`; `delayMs` | one `WM_CHAR` per UTF-16 unit |
| `mouse` | `action` move\|down\|up\|click\|wheel; `x`,`y` (client px) or `rx`,`ry` (fractions), or `ref` + `snapshot` (a `ui` node); `button` left\|right\|middle; `notches`; `holdMs` | moves the virtual cursor (if a point is given), then buttons/wheel on the virtual GameInput mouse |
| `raw` | `action` move\|down\|up\|click\|wheel; `dx`,`dy` (mouse counts); `steps`; `button`; `notches`; `holdMs` | virtual GameInput mouse: camera while the mouse is grabbed |
| `release` | | releases every held key and button, whoever pressed it, and ends the `input` move override |
| `focus` | | re-sends the "focus gained" messages |
| `window` | `clientWidth`, `clientHeight` | never activates the window; it stays off screen |
| `flush` | | waits until everything posted was dispatched |
| `screenshot` | `format` png\|jpeg; `quality`; `maxWidth` (960); `region{x,y,w,h}` | `width`, `height`, `format`, `data` (base64). `PrintWindow(PW_RENDERFULLCONTENT)`, so it works while the window is covered |
| `ui` | read: `depth`, `scope`, `includeInvisible`, `maxNodes`; act: `snapshot`, `action` click\|setText\|scroll, `ref`, ... | read: `snapshot`, `screens`, `active`, `roots`, `nodes{ref: {role, name, text, value, state, bounds, boundsNorm, actions, children, source, hash}}`, `truncated`. JSON UI screens in full; an Ore UI screen is one opaque node (`source.kind:"oreui"`). Codes: `stale_ref`, `stale_snapshot`, `occluded`, `not_ready`, `unsupported` |
| `manager` | `action` attach | the manager window attaches to the game; see [The manager](#the-manager) |
| `close` | | closes the game (`WM_CLOSE` to its window) |
| `join` | `host`, `port` (19132), `wait`, `timeoutMs` (60000) | the client joins a server by address through the game's own external-server join (`ClientInstance::connectToThirdPartyServer`), from the title screen or from inside a world, which it leaves first. Since 1.26.60 it answers the two prompts in the way, as a player would with the keyboard: "Online play is not rated" (Proceed, then it connects again) and "Trust this server?" for the server it joins (Trust and Join). Replies `started:true` once it connects, with `wait:true` `joined:true` once it is in the level; `left`: a world was left first. Codes: `join_failed` (the client gave up and shows why), `timeout` (`screen`: the game may still be connecting), `busy` (a join is in progress), `not_allowed` (a host off the PC that `loopbackOnly` keeps the game from: not in `allowHosts`), `bad_args`. Without `host`: `mode`, `connection` none\|local\|remote, `host`, `port`, `inLevel`, `joining` (the attempt in progress), `last` (how the last one ended). A server off the PC needs Remote mode (config `allowHosts`) |

#### Several connections

Several clients can be connected at once (config `wsMaxClients`, default 8), each served by its own
thread, in order. Ops that post window input are serialized, so two clients never interleave a
press or a typed text; `ping`, `state`, `tap`, `world`, `look`, `input`, `send`, `stack`,
`timescale`, `join` and the `ui` read run without that lock (they only read, or hand their work to
the game threads). What a client holds is released when it disconnects (or its process dies), and
nothing another client holds: its keys and mouse buttons, the `button` holds it made and the
`input` override it set.

For units with per-client state: `currentClient()` is the id of the connection the current thread
serves (from 1, never reused; 0 on the game thread). Tag the state with it and release it from
`clientGone()` in `src/Pipe.cpp` (as `clearMoveInputOf` and `releaseGameButtonsOf` do), or register
`onClientGone(fn)` in `enable()`: either runs on the departing connection's thread, after its keys
and buttons are released, with input ops serialized (not at shutdown).

#### Ops for bedboo (transport `bedd`)

Each of these is on by default; with its config key set to `false` it replies
`{"ok":false,"code":"disabled","key":"<key>"}` (the `ui` op likewise, key `ui`). Errors may carry `code`: `disabled`,
`not_in_level`, `refused`, `timeout`, `bad_args`, `busy`. An op this build lacks answers
`unknown op 'x'`. A client finds what a game has by calling each op once without arguments:
`unknown op` = absent, `code:"disabled"` = present but off, `ok` = present. The no-argument form
of every op is read-only.

| op | key | no-argument reply | action form |
|---|---|---|---|
| `tap` | `tap` | `enabled`, `attached` (a connection is subscribed), `link{state up\|down, type remote\|local, address}`, `journal{frames, bytes, complete}` (the join journal now), `lastReplay` (what the last reader was replayed), `replay` (a journal is installed), `counters{in, out, dropped, sent, readers}`, `hooks` | `action:"subscribe"`: this connection receives the tap ([Tap protocol](#tap-protocol)) |
| `world` | `gameHooks` | `inLevel`, `levelEpoch` (+1 per world joined), `topScreen` (e.g. `hud_screen`, `pause_screen`), `mouseGrabbed`, `showingMenu`, `inputMode` mouse\|touch\|gamepad\|undefined, `player{runtimeId, eye{x,y,z}, rot{yaw,pitch}, slot}` (null outside a world), `tick` (client ticks seen) | (none) |
| `look` | `gameHooks` | `{inLevel, yaw, pitch, tick}` | `yaw`, `pitch`, `timeoutMs` (1000\*, at most 60000) |
| `input` | `gameHooks` | `active`, `via` hooks\|keys, `hooks{raw,state}`, `tick`, `applied`, `ticksLeft` (a `ttlTicks` override) | `move{x,z}`, `jump`, `sneak`, `sprint`, `ttlMs` (300) or `ttlTicks` |
| `slot` | `gameHooks` | `{slot}` (null outside a world) | `slot` 0..8 |
| `action` | `gameHooks` | `{names}` | `name` drop\|dropAll\|inventory\|perspective\|pause |
| `button` | `gameHooks` | `{held}` | `button` attack\|use\|pick; `action` down\|up\|click; `holdTicks`; `releaseOn` blockChange |
| `send` | `allowSend` | `{allow}` (the ids in `sendAllow`) | `buf`, `timeoutMs` (2000\*, at most 10000) |
| `stack` | `allowStack` | `{enabled, via:"raw", stats{sent, answered, rejected, failed, resyncs}}` | `buf`, `timeoutMs` (2000\*); or `view:true` |

\* And `timeoutMs` (1000) of `slot`, `action` and `button`: these defaults hold at 20 ticks a second
and faster. When the client ticks slower (a slower game speed), a request without `timeoutMs`
waits that much longer (the default times the measured tick interval over 50 ms, up to the op's
maximum); a `timeoutMs` the caller passes is used as given. Likewise "the client is in a world"
(`not_in_level` of `slot`, `action`, `button`, `send`, `stack`) means a client tick within
20 tick intervals, at least 1 s.

- **`world`** is a snapshot the client thread takes: the screen fields every frame (a
  `ClientInstance::$update` post-hook, because a paused singleplayer world does not tick), the
  player every client tick. The connection's thread only copies it.
- **`look`** turns the player with `LocalPlayer::localPlayerTurn` on client ticks and replies
  `yaw`, `pitch`, `tick`, `turns` once both are within 0.5 degrees. Degrees as in
  player_auth_input: yaw 0 = +Z, 90 = -X; pitch -90 = up, clamped to +-89.9 like the game does.
  `localPlayerTurn(d)` turns 1:1 in degrees with the axes swapped (`{-dPitch, dYaw}`), and the turn
  lands after the tick that requested it. Errors: `not_in_level`, `timeout` (with the current
  `yaw`/`pitch`; a paused singleplayer world does not tick), `busy` (a newer `look` replaced it),
  `bad_args`.
- **`input`** moves the local player inside the game's own input pipeline, so the client builds
  every `player_auth_input` itself. `move` is player_auth_input's move vector (`x` = left - right,
  `z` = forward - back, each clamped to [-1, 1]; a diagonal is normalized). The override is a
  dead-man switch: it ends `ttlMs` (at most 60000) after the request, so a controller refreshes it
  while it wants to keep moving; `ttlMs:0`, `release` and the owning client disconnecting end it at
  once. `ttlTicks` (at most 1200; not with `ttlMs`, that is `bad_args`) ends it after that many
  client ticks instead, the same game time at any game speed; it also ends once the client stops
  ticking for 5 s (out of the world, paused), is `not_in_level` when asked for while it is not
  ticking, and the probe reports the ticks left as `ticksLeft`.
  - `via:"hooks"`: post-hooks on `ClientInputUpdateSystem::extractRawHIDInput` (writes
    `RawMoveInputComponent::mRawInput`/`mRawMove`) and `inputHandlerUpdatePlayerState` (writes
    `MoveInputComponent::mInputState`/`mRawInputState` before and after the original:
    `MoveInputState` flags `SneakDown`=0, `JumpDown`=7, `SprintDown`=8, `Up`..`Right`=13..16 and
    the jump/sneak input edges 21..26, plus `mAnalogMoveVector`). Applied only while the mouse is
    grabbed and no screen shows; the key bindings do not matter. The keyboard handler changes those
    flags only on key events, so on the first tick without the override they are cleared once.
    `tick` counts `inputHandlerUpdatePlayerState` calls, `applied` the ticks the override was written.
  - `via:"keys"` (a hook did not install): the default keymap's virtual keys (W/A/S/D, SPACE,
    LSHIFT, LCTRL) are held until the deadline. They go through the window like `key`, so they
    also reach an open screen, and a rebound key breaks them.
- **`slot`, `action`, `button`**: the connection's thread queues them and replies once the next client
  tick applied them (`timeoutMs`, default 1000): `{"ok":true,"tick":n,...}`, or `code`
  `not_in_level`, `busy` (a screen is open; `button up` still works), `timeout`,
  `bad_args`, `refused`. A paused singleplayer world does not tick: after `action pause` there,
  resume with the `key` op (ESC).
  - `slot`: the hotbar key `1`..`9` of the default key map, replied once the player holds the
    slot. The client sends `mob_equipment` itself. Replies `slot` and `via` keys\|none (already held).
  - `action`: the key of the default key map, released on the next tick: drop Q, dropAll Ctrl+Q,
    inventory E, pause Escape; perspective calls `ClientInputCallbacks::handleTogglePerspectiveButtonPress`
    (since 1.26.60 the game exports none of the other `handle*ButtonPress`). A rebound key breaks them.
  - `button`: left/right/middle on the virtual GameInput mouse, released
    by the tick listener after `holdTicks` (click: default 2) or, with `releaseOn:"blockChange"`,
    once the block the client's hit result pointed at when it went down changes, turns to air or is
    no longer targeted (`refused` when no block is targeted). `up` applies at once. Never blocks
    the connection. Replies `held`, and `block{x,y,z}`.
- **`send`**: `buf` is the base64 of one serverbound packet (varint header + body, as
  bedrock-protocol serializes it). It is rebuilt on the client thread
  (`MinecraftPackets::createPacket` + `Packet::read` with the client NetworkSystem's
  `ReflectionCtx`) and sent through the local player (`sendNetworkPacket`), on a remote server and
  in singleplayer alike; the reply `{packetId, name}` comes once it is sent. Bytes the game does not
  read are `bad_args` and nothing is sent; an id not in `sendAllow` is `refused`. `state` reports
  `stats.send{sent, refused, failed, lastError}`.
- **`stack`**: see below.

**Security.** The token is the boundary. Anything that holds it can make the client send packets
as the signed-in player (`send`), move, craft, drop and destroy the player's items on whatever
server the client is on (`stack`), and read the player's whole session (`tap`). The token is
random per game start, written only to the discovery file under the user's `%LOCALAPPDATA%`, and
compared in constant time; the server listens on loopback only and refuses browser origins that
`allowOrigins` does not list, so other users, other machines and web pages cannot reach it. `send`
stays limited to `sendAllow` (default text 9 and command_request 77: chat and commands, as the
player could type them). Ids 1 (login), 30 (inventory_transaction), 144 (player_auth_input) and 147
(item_stack_request) are always refused, even when listed: the client owns them, and forged ones
would fight its prediction and bookkeeping. Widen `sendAllow` only for a server you control. Set
`allowSend`, `allowStack`, `tap`, `gameHooks` or `ui` to `false` to take their ops away.

#### `stack`: item stack requests through the client (`allowStack`)

| Form | Reply |
|---|---|
| `{op:"stack"}` (probe) | `{ok, enabled:true, via:"raw", stats:{sent, answered, rejected, failed, resyncs}}` |
| `{op:"stack", buf, timeoutMs=2000}` | `{ok, via:"raw", requestIds:[client ids], map:{"<caller id>": client id}}` |
| `{op:"stack", view:true}` | `{ok, inventory:[...], ui:[...], open?:{containerId, type, items:[...]}}`: what the client shows, each `{slot, item, count, stackId}` |

- `buf` is the base64 of one whole item_stack_request packet (id 147), varint header plus body, as
  bedrock-protocol serializes it. Any other packet id, and bytes the game does not read, are `bad_args`.
- On the next client tick the packet is rebuilt with `MinecraftPackets::createPacket` and
  `Packet::read`, and the local player sends it.
- Request ids: each request gets a new client id, and the reply comes back once the packet is sent. Ids
  are odd, negative and counted down from `-0x40000001`, far below the client's own `-1, -3, ...`.
  Every slot stack id that names a caller id is renamed too. That covers the request's own id (a
  `consume`, or the crafted output) and an earlier request of the same packet or of an earlier one
  (the last 256 ids are remembered). `map` keys are the caller's ids as strings.
- The server answers with the client ids in item_stack_response, which bedboo sees on the tap. The
  answer can only arrive after the packet is sent, but it can come before the reply to `stack` is
  read, so the caller holds an unmatched response until the reply arrives.
- `via:"raw"`: the request does not go through the client's `ItemStackNetManagerClient`. In this game version
  the manager has no callable symbol (every member is MCNAPI in LeviLamina's headers), and its
  predictions come from the screen controllers moving items, not from request actions. So the
  client predicts nothing for these requests.
- Resync: when the server accepts one of these requests (hooked
  `ClientNetworkHandler::handle(ItemStackResponsePacket)`), the client sends
  `inventory_transaction {type: inventory_mismatch}` on its next tick. That is the packet the client
  itself sends when its inventory is out of step, and the server answers with the whole inventory.
  The client then shows the server's state, open container included. A rejected request changes
  nothing on either side, so it gets no resync.
- The server checks every request (stack ids, open screen, recipe). A screen the request needs (the
  inventory, a chest) must already be open in the game: open it with `action inventory`, `key E` or
  by using the block.

#### `timescale`: game speed

Not behind a config key. It changes the speed of a world this game hosts (singleplayer, or a LAN
world it hosts): the client and its local server, together.

| Form | Reply |
|---|---|
| `{op:"timescale"}` (probe) | `{ok, scale, paused, source, tps:{client, server}, ticksPerSecond, effective:{client, server}, local, inLevel}` |
| `{op:"timescale", scale?, paused?, timeoutMs=2000}` | the probe's fields, once the client and the local server both run at it |

- `scale` is clamped to 0.05..20 (1 = normal, 20 ticks a second). `paused:true` stops the ticks of both sides
  (`Minecraft::setSimTimePause`) and `false` resumes them. The game's own pause menu pauses and resumes the same
  way.
- `source`: `default` (1, nothing set), `config` (`timeScale`, applied on entering the world), `local` (set over the
  WebSocket), `server` (a server running BeDD set it).
- `tps.client`, `tps.server`: ticks of the client and of the local server per second, measured over the last 2 s (so
  they trail a change by up to 2 s; 0 while paused). `tps.server` is `null` on a remote server. `ticksPerSecond`: the
  client timer's base rate (20). `effective.client` / `effective.server`: `{scale, paused, ticksPerSecond}` as each
  side's Minecraft reported them after it last applied a change (`null` before the first).
- Each side is changed on its own thread: the server's through `ServerInstance::queueForServerThread`, the client's
  after the next `ClientInstance::update` (frames go on while the ticks are paused). The scale is written to the sim
  timer (`Timer::mTimeScale`) directly: `Minecraft::setSimTimeScale` multiplies the current scale and caps it at 10,
  so it is not idempotent. A timer catches up only a few ticks per frame, so `tps` shows what the game achieves. With
  a local world, client and server alike: 0.05 → 1, 0.5 → 10, 2 → 40, 3 → 60, 5 → 100, 10 → 200, 20 → 400 ticks a
  second; the same scale set again changes nothing.
- Leaving a world puts the client back to 1; entering one applies `timeScale` (default 1), so nothing of the last
  world carries over.
- Errors: `not_in_level`; `remote` (the world is on a server this game does not host: its speed cannot change from the
  client); `server_led` (that server runs BeDD and has sent its scale: change it there); `timeout` (a side did not
  apply it in time: it still does once its thread runs, and `effective` shows it); `refused`; `bad_args`.
- Server-led: a server running BeDD sends each player a level_event packet (`event_id` 3007 SimTimeScale, position
  0,0,0, `data` = the scale's float bits as int32, 0 while the server is paused) when its scale or pause changes and
  when the player joins. The client applies it (hooked `ClientNetworkHandler::handle(LevelEventPacket)`, after the
  game's own handling, and only where the game did not already reach that scale); 0 pauses the client, keeping its
  scale for the resume.

#### Who sends what (bedboo transport `bedd`)

The real client owns every serverbound packet; bedboo turns what it would have sent into intents.
This table is the same as bedboo's `lib/bedd/authority.js`.

| Class | Packets | Handling |
|---|---|---|
| clientOwned | login, client_to_server_handshake, request_network_settings, resource_pack_client_response, client_cache_status, client_cache_blob_status, request_chunk_radius, subchunk_request, serverbound_loading_screen, set_local_player_as_initialized, network_stack_latency, tick_sync, player_skin, emote_list, respawn, player_action (respawn / dimension_change_ack / start_item_use_on / stop_item_use_on), animate, interact mouse_over_entity | bedboo drops them (counted): the client sends its own. A respawn is the user clicking Respawn on the client |
| translated | player_auth_input (move_vector + flags to `input`; yaw/pitch to `look`; block_actions start_break/abort_break/stop_break to `button attack`), mob_equipment (`slot`), inventory_transaction (item_use click_block/click_air, item_release, item_use_on_entity: `look`, then `button use`/`attack`), interact open_inventory (`action inventory`), container_close (key ESC), item_stack_request (`stack`) | bedboo's router turns them into ops |
| shared | text, command_request | sent through `send`; fallback: T or SLASH, then `text`, then ENTER |
| unsupported | book_edit, block_pick_request, crafting_event, set_default_game_type, everything else | bedboo throws BeddUnsupportedError (ERR_BEDD_UNSUPPORTED) |

item_stack_request is unsupported only when the `stack` probe does not answer `ok`.

### Tap protocol

The tap travels on the game's WebSocket (config `tap`, on by default). A client sends
`{"op":"tap","action":"subscribe"}`, gets `{"ok":true}`, and from then on that connection
receives **binary** messages, one tap frame each; it may still send requests and get text replies
on the same connection. One subscriber at a time: a second gets `code:"busy"` until the first
disconnects. It carries every clientbound game packet the client receives, an echo of every
serverbound packet it sends (not login, the handshakes or request_network_settings), and JSON
markers. bedboo's `transport: 'bedd'` subscribes to it.

Frames, little-endian (each binary message is one whole frame):

```
u32 len (bytes after this field) | u8 kind | u8 flags | u16 0 | u64 seq | u64 tUs (steady clock µs) | payload
```

`len` is at most 16 MiB + 20. `seq` rises over the process (replayed frames keep theirs).

| kind | | payload |
|---|---|---|
| 1 | CLIENTBOUND | one game packet: varint header (`id \| senderSub<<10 \| targetSub<<12`) + body, as bedrock-protocol's `parsePacketBuffer` reads it |
| 2 | SERVERBOUND_ECHO | the same, for a packet the client sent (never ids 1, 3, 4, 193) |
| 4 | MARKER | UTF-8 JSON |

Readers ignore other kinds. Flags: 1 replay (from the join journal), 2 reserialized
(`Packet::write`, not wire bytes), 4 local link, 8 suppressed (reserved).

| marker | fields |
|---|---|
| `HELLO` (always first) | `tap:1`, `mod`, `game:"1.26.60.05"`, `protocol` (2225), `pid`, `link{state, type, address}`, `journal{frames, bytes, complete}` (what this reader is replayed) |
| `LINK_UP` | `type` remote\|local, `address` |
| `LINK_DOWN` | `reason` |
| `REPLAY_BEGIN` | `frames` |
| `REPLAY_END` | `complete` (false when the journal went past `tapJournalMiB`) |
| `DROPPED` | `count`; the connection closes after it |

A reader gets HELLO, REPLAY_BEGIN, the join journal's frames (flag replay), REPLAY_END, then the
live stream; live frames that arrive during the replay wait until REPLAY_END, and live frames the
replay already held are skipped by seq. Without a journal the pair is empty (`complete:false`). A
reader more than 32 MiB behind gets DROPPED and is disconnected; subscribing again gives a full replay.

All publishers share `src/TapSink.h`, which must stay byte-identical: each installs its sink with
`prev = gEmit.exchange(&own)`, forwards every frame to `prev`, and on removal puts `prev` back
only if `gEmit` is still its own (compare-exchange). The hooks only copy bytes; the tap's writer
thread drains its queue into the subscriber's connection.

#### Remote servers (`NetTap.cpp`)

The client's NetworkSystem follows its one open connection: `NetworkSystem::send` (before the
client's first packet goes out) and `runEvents` (every tick) look it up, so a new one brings the
link up and none takes it down; `$onConnectionClosed` and `closeConnection` end it at once (since
1.26.60 the game exports no `onNewOutgoingConnection`, `onAllConnectionsClosed` or `disconnect`). On
a remote link, `BatchedNetworkPeer::_receivePacket` of its batched peer (after it, `HasData`) gives each decompressed,
decrypted packet, and `NetworkSystem::_sendInternal` each serialized packet the client sends: wire
bytes, flags 0. With config `trace`, the first 50 packets of each link are logged as
`tap trace #n in|out id len`: they come in the order of a BDS capture (network_settings,
server_to_client_handshake, play_status, resource_packs_info, ..., start_game, ...).

**Blob cache.** A client with its blob cache on gets sub-chunks as blob hashes and only the blobs
it misses (`client_cache_miss_response`); the rest come from its cache on disk and never cross the
wire, so a reader could not build the world. While the tap is on, BeDD therefore clears `enabled`
in the client's `client_cache_status` before it is sent, and servers send every sub-chunk in full.
The echo shows `enabled:false`.

#### Singleplayer and LAN-host worlds (`TapLoopback.cpp`, config `tapLoopback`)

`tapLoopback` (default `true`, effective only with `tap`) puts singleplayer worlds and LAN worlds
the client hosts on the tap. The client then talks to its in-process server through the
LocalConnector and the remote capture publishes nothing; `LoopbackPacketSender` is hooked instead,
and each packet is re-serialized (varint header `id | senderSub << 10 | targetSub << 12`, then
`Packet::write` with the sending `NetworkSystem`'s `ReflectionCtx`). Frames carry the flags
`reserialized` (2) and `local` (4).

- **CLIENTBOUND**: the server's sender (`$sendToClient` both overloads, `$sendToClients`,
  `$sendBroadcast` both overloads, `$sendTo`; `$sendTo` and `$sendBroadcast(except)` have no symbol
  since 1.26.60 and are found in `LoopbackPacketSender`'s vtable, next to the slots the game still
  exports), only for a `NetworkIdentifier` whose connection on
  the server is of type `Local`. LAN guests are remote connections and are never published
  (counted as `guest`). Broadcasts are published once the local player got `start_game`.
- **SERVERBOUND_ECHO**: the client's sender (`$sendToServer`, `$send`, `$sendTo`), never ids 1, 3, 4
  or 193.
- Chunks: the integrated server answers every `subchunk_request` through the same sender, so each
  `subchunk` and `subchunk_request` the client exchanges is on the tap (checked in game: as many
  `subchunk` frames as `ServerNetworkHandler::handle(SubChunkRequestPacket)` calls). The client
  asks relative to one origin (its own sub-chunk) and only up to each column's
  `highest_subchunk_count`; a local client sends `client_cache_status` with `enabled:false`, so
  every entry carries its payload.
- Only while the client's link (`tap::gLinkType`) is local. The client sender's first packet on a
  new connection and `$onConnectionClosed` on the client `NetworkSystem` set it from the connection type, as the tap
  core does, so the loopback tap works without it. While no link is known yet (the in-process
  server already sends before the client's connect call returns), a `Local` connection of the
  client counts; that check never writes `gLinkType`. The remote capture only publishes on a
  `Remote` link (`_sendInternal` also runs for the local link), so no packet is published twice. A
  sender method that calls another publishes a packet once.
- Without the tap (`tap::gEmit` null) frames are only counted. `state` reports
  `stats.tapLoopback{enabled, hooks, link, clientbound, echo, bytes, unpublished, guest}`.
- With `trace`, the first 50 frames are logged (`trace tap loopback: in|out id= len=`) and the first
  4000 are written to `mods\BeDD\data\tap-loopback.bin` in the tap's framing (seq and tUs 0).

#### Join journal (`TapJournal.cpp`)

With `tap` on, the journal chains onto `bedd::tap::gEmit` and sets `bedd::tap::gReplay`, which the
tap calls for each new subscriber between `REPLAY_BEGIN` and `REPLAY_END`. Replayed frames keep
their seq and order and carry flag `replay`.

- Only CLIENTBOUND frames are kept. The journal resets on a new link (the `LINK_UP` marker or a
  change of `gLinkType`) and on `start_game` (11), which becomes its first frame.
- Compaction keys (read from the leading fields only; the last frame with a key wins):
  `level_chunk` (x, z, dimension), `update_block` (position,
  layer), `set_time`, `set_health`, `network_chunk_publisher_update`, `current_structure_feature`,
  `inventory_content` (window), `inventory_slot` (window, slot), `level_event` (event id, so the
  weather keeps its last state), `update_abilities` (unique id), the `add_*` packets (unique id;
  `add_player` by runtime id), and per runtime id `set_entity_motion`, `move_entity`,
  `move_player` and `mob_armor_equipment`. Packets that carry only the fields that changed are
  keyed by entity and the set of fields, so every field keeps its last value: `set_entity_data`
  (the metadata keys), `update_attributes` (the attribute names), `move_entity_delta` (the fields
  present) and `mob_effect` (the effect).
- `subchunk` is kept per sub-chunk, not per packet: the client asks for all its sub-chunks relative
  to one origin, so a packet is dropped only once newer frames carry every sub-chunk in it (origin +
  each entry's offset). A packet the journal cannot walk is kept whole.
- `remove_entity` (14) deletes the entity's `add_entity`/`add_item_entity`/`add_player` frame
  (`add_player` is matched by its unique id's bytes) and everything keyed by its runtime id.
- Not kept: `text`, `animate`, `network_stack_latency`, `play_sound`, `stop_sound`,
  `level_sound_event` (and v2), `spawn_particle_effect`, `level_event_generic`.
- Past `tapJournalMiB` (default 128) frames are no longer kept and the journal reports
  `complete:false` until the next reset: a late reader then misses chunks until the player
  re-enters them or the client rejoins.
- `state` reports `stats.journal`: `enabled`, `frames`, `bytes`, `complete`, and the counters
  `seen` (clientbound frames), `compacted` (replaced by a later frame with the same key), `pruned`
  (entities whose add frame `remove_entity` deleted), `transient` (not kept), `overCap` (not kept
  past `tapJournalMiB`), `resets` and `replays`.

#### Server target (client+server mode)

BeDD built with `--target_type=server` runs inside a Bedrock Dedicated Server with LeviLamina and
serves the same WebSocket protocol (its discovery file says `kind:"server"`; `BeDD.exe server
start` waits for it and `BeDD.exe ps` lists it). The server leads the
game speed: it broadcasts its scale to every player as a LevelEvent packet (`mEventId` 3007,
SimTimeScale; `mPos` 0,0,0; `mData` the bits of the float scale, 0 while paused), whenever it or
the pause changes and to each player once it has spawned; BeDD on a client follows it. Other ops answer `unknown op`.

| op | fields | reply |
|---|---|---|
| `ping` | | `protocol`, `pid`, `target:"server"` (no `hwnd`) |
| `state` | | `target:"server"`, `clients`, `loopbackOnly`, `stats{loopbackRebinds, serverQueriesRefused}` |
| `server` | | `tick` (the level's current tick), `tps` (ticks measured over the last 2 s), `paused`, `port` (`server-port`), `players[{name, xuid}]` |
| `timescale` | none: probe | `scale`, `paused`, `source` default\|config\|local, `tps{client:null, server}` (measured), `ticksPerSecond` (what the scale asks for: 20 × scale, 0 while paused) |
| `timescale` | `scale` 0.05..20 and/or `paused` true\|false; `timeoutMs` (5000) | as the probe, after the change. Runs on the server thread (`ServerInstance::queueForServerThread`, which also runs while paused): the scale goes straight into the simulation timer (`Minecraft::setSimTimeScale` multiplies and caps at 10), the pause through `setSimTimePause` |
| `command` | `command` (a leading `/` is optional); `timeoutMs` (10000) | `success`, `successCount`, `output[]` (the translated lines), run as the server console would |

Errors: `not_in_level` (the server has not started), `bad_args`, `timeout` (the server thread did
not answer). `tps.server` is what the server achieves: at a high scale it may stay below
`ticksPerSecond` when a tick takes longer than the scale allows.

### The manager

The manager window is `BeDD.exe` started with no arguments, one per logon session; `BeDD.exe
launch` starts it when none runs, and `BeDD.exe manager status|stop` talk to it. There is no
manager pipe: the window is just another WebSocket client of each game.

- It watches `%LOCALAPPDATA%\BeDD\run\` for discovery files and connects to every game (and
  server) it finds, with the token from the file.
- On each game's connection it sends `{"op":"manager","action":"attach"}`, then polls `state` for
  the tiles' labels and input counters. `label` comes from the game's `BEDD_LABEL`.
- While a connection is attached, the game keeps its window off screen, off the taskbar and out
  of Alt+Tab; the manager shows it through a DWM thumbnail.
- With config `manager` (default `true`) a game that no connection attaches within 30 s of its
  window existing, or whose attached connection drops, closes itself (logged in `latest.log`), so
  closing or killing the manager leaves no game behind. `manager:false` keeps the window hidden and
  never closes the game for want of a manager (CI, tests of the protocol itself).
- Closing a game from the window sends `{"op":"close"}` (`WM_CLOSE` to the game window); closing
  the window closes every game it attached.

## License

By contributing you agree that your contributions are licensed under the [LGPL-3.0-or-later](COPYING.LESSER), the license of this project.
