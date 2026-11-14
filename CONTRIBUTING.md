# Contributing to Be.D.D.

Rules for everyone changing this repository, humans and agents. The protocol, the tap and the internals are
documented once, on the docs site: [WebSocket reference](https://docs.levimc.org/BeDD/reference/websocket)
(`docs/reference/websocket.md`), [Tap](https://docs.levimc.org/BeDD/reference/tap) (`tap.md`),
[UI tree](https://docs.levimc.org/BeDD/reference/ui) (`ui.md`),
[How it works](https://docs.levimc.org/BeDD/reference/how-it-works) (`how-it-works.md`),
[Building](https://docs.levimc.org/BeDD/reference/building) (`building.md`: full setup with winget, dev-loop flags).

## Bug reports and features

Use the [issue templates](https://github.com/LiteLDev/BeDD/issues/new/choose). A bug report includes: the Be.D.D.,
LeviLamina and Minecraft versions; the request sent (JSON, or the `BeDD.exe` command line) and what the game did
instead; `mods/BeDD/config/config.json`; the relevant part of `logs/latest.log`, ideally with `"trace": true` (it
logs which input reached the game and whether it was synthetic or real).

## Development

Two parts, which talk only over the mod's WebSocket: the mod (C++ in `src/`, loaded by LeviLamina in the game or in
a Bedrock Dedicated Server) and the app `BeDD.exe` (C# in `app/`), which sets up, starts and shows the games.

### Source layout

| Mod file | Holds |
|---|---|
| `src/BeDD.h` | shared state and declarations; a new unit declares its own things in its own header |
| `src/Mod.cpp` | entry point, config, virtual key/cursor state |
| `src/Window.cpp` | window discovery, subclassing, synthetic event dispatch, focus shield, window kept off screen |
| `src/ApiHooks.cpp` | user32 hooks (cursor, key state, foreground, activation, raw input) |
| `src/GameInputHooks.cpp` | GameInput v2 hooks (virtual mouse, keyboard readings) |
| `src/Audio.cpp` | FMOD hooks: always silent (NOSOUND output, master channel group muted) |
| `src/NetHooks.cpp` | loopback-only network (`loopbackOnly`, `allowHosts`); also in the server target |
| `src/ServerTimeScale.cpp` | server target only: `server`, `timescale`, `command` ops, config `timeScale`, LevelEvent 3007 to players |
| `src/Join.cpp` | `join` op (the game's external-server join, client thread; `host: null` leaves the world); where the client plays, for `state` |
| `src/WsServer.cpp`, `.h` | WebSocket server: Winsock on 127.0.0.1, RFC 6455 handshake and framing, token and `Origin` checks, one thread per connection |
| `src/Discovery.cpp`, `.h` | writes `%LOCALAPPDATA%\BeDD\run\<pid>.json` (`BEDD_HOME`) while listening, deletes it at exit |
| `src/Pipe.cpp` | the protocol: `handle()` dispatches every op (one `else if` per op), `concurrentOp()`, per-connection ownership (name is historical) |
| `src/Manager.cpp` | `manager` and `close` ops; closing a game no manager attaches |
| `src/UiJson.cpp` | `ui`: reads the scene stack (JSON UI control trees) on the client thread |
| `src/UiTree.cpp`, `UiTree.h` | `ui`: client-thread job queue, node fields every reply uses |
| `src/UiView.cpp` | `ui`: builds the replies (text view or JSON), keeps the refs, maps Ore UI DOM to roles |
| `src/UiActions.cpp` | `ui` click/setText/scroll and `mouse` with `ref`: resolves a ref, drives the input ops |
| `src/UiOre.cpp`, `UiOre.h` | `oreui` op: Ore UI DOM read through the game's V8 (also used by `ui`) |
| `src/Screenshot.cpp`, `.h` | `screenshot`: `PrintWindow(PW_RENDERFULLCONTENT)`, encoded with WIC |
| `src/Trace.cpp` | trace-mode input logging |
| `src/TickCadence.cpp` | measured client tick interval, so tick waits and default timeouts follow the game speed |
| `src/MoveInput.cpp` | `input` op: move override in `ClientInputUpdateSystem`, dead-man TTL (`gameHooks`) |
| `src/Send.cpp` | `send` op (`allowSend`) |
| `src/GameState.cpp`, `src/Look.cpp` | `world` snapshot, absolute `look` (`gameHooks`) |
| `src/GameActions.cpp` | `slot`, `action` (incl. `swing`), tick-based `button`, `settings` (`gameHooks`) |
| `src/Stack.cpp` | `stack` op (`allowStack`) |
| `src/TimeScale.cpp` | `timescale` op and `timeScale` on the client and its local server; a BeDD server's scale (level event 3007) |
| `src/TapSink.h` | frame type and sink chain every tap publisher shares |
| `src/Tap.cpp`, `Tap.h` | the tap: framing, markers, writer thread, `tap` op, subscriber |
| `src/NetTap.cpp` | tap capture on a remote link; blob cache switch |
| `src/TapLoopback.cpp` | tap capture in singleplayer and LAN-host worlds (re-serialized) |
| `src/TapJournal.cpp` | the tap's join journal |
| `src/MemoryOperators.cpp` | routes `new`/`delete` through LeviLamina's allocator (required) |

App: `app/BeDD.slnx` = `app/BeDD/BeDD.csproj` (.NET 10, `WinExe`, NativeAOT, Win32 via CsWin32 and
`app/BeDD/NativeMethods.txt`) + `app/BeDD.Tests` (xunit v3). `Program.cs` opens the manager window without
arguments, else runs the command line attached to the parent console. `app/BeDD/Cli/`: `Dispatcher`, `Registry`,
`Help`, `Ctx` (output), one file per command or small group. `app/BeDD/Core/`: shared by commands and window
(`Paths`, `Discovery`, `Instances`, the WebSocket client, launching, mod install, servers, `BuildInfo`).
`app/BeDD/Gui/`: the manager window (`ManagerApp`, DWM thumbnails, attached over each game's WebSocket).

Other: `spec/bedd.asyncapi.yaml` (protocol), `tests/ws/`, `tests/installer/`, `packaging/wix/` (`Package/` MSI,
`Bundle/` setup.exe), `tools/showcase/`, `docs/` (site, `docs/zh/` Chinese), `.github/workflows/` (`build.yml`,
`release.yml`, `docs.yml`), `.github/actions/stage`.

### Building

| What | Needs | Command |
|---|---|---|
| mod, client | Windows x64, xmake, VS Build Tools with MSVC 14.4x+, LLVM `clang-cl` on `PATH` (`xmake.lua` sets toolchain `clang-cl`) | `xmake f -P . -y -m release -p windows --target_type=client` then `xmake -P . -y` → `bin\BeDD\` (DLL, manifest) |
| mod, server | same | `--target_type=server` → `bin\BeDD-server\` (platform `server`); `BeDD.exe server init` installs it into the server's `mods\BeDD-server\` |
| app | .NET 10 SDK | `dotnet build app/BeDD.slnx` |
| app, NativeAOT | + VS C++ tools, `vswhere.exe` on `PATH` (`%ProgramFiles(x86)%\Microsoft Visual Studio\Installer`) | `dotnet publish app/BeDD/BeDD.csproj -c Release -o bin\app` |
| installers | WiX from NuGet; release layout in `bin\release\BeDD` (`.github/actions/stage`) | `dotnet build packaging/wix/Bundle/BeDD.Bundle.wixproj -c Release -p:Platform=x64` |
| docs | Node 22.12+ (CI: 24) | `cd docs; npm ci; npm run build` |

- Always pass `-P .` (in a git worktree xmake otherwise builds the main checkout's `xmake.lua`).
- The mod version comes from `git describe`: the repository needs a commit (and its tags in a shallow clone).
- The server target holds the WebSocket server, loopback-only network and server ops only; `xmake.lua` lists its
  files (the rest of `src/` needs the client's LeviLamina headers).
- `xmake-requires.lock` pins both targets, but xmake rewrites it with the configured target's pins only:
  `git checkout -- xmake-requires.lock` before configuring the other target and before committing (CI does too).
  To move a pin, configure each target and commit the union of both targets' entries.

### Testing

- `dotnet test app/BeDD.slnx`.
- `node --test tests/ws/` (Node 24): protocol tests against a running game, found through its discovery file
  (`BEDD_PID` picks one, else the newest live one); skipped without a game.
- `node --test tests/installer/installer.test.mjs`: installs, upgrades, uninstalls the installers.
- `npm test` in `tools/showcase/`.
- In game: `bin\app\BeDD.exe dev` clones the base instance for slot 9 (`--slot`), installs `bin\BeDD`, launches in
  the background and logs every foreground and cursor-clip change to `tools\out\desktop-watch.log` until Ctrl+C,
  `tools\out\dev.stop` or game exit (`tools\out\dev.restart` reinstalls and relaunches). A line ending in
  `MINECRAFT HAS THE FOREGROUND` means a game took the focus: a bug. Check the ops work, the game never takes the
  foreground and real input never reaches it.
- CI (`build.yml`) builds both targets, checks `BuildInfo`, runs `dotnet test`, publishes, builds and tests the
  installers and validates the AsyncAPI spec. It must pass before merge.

### Code rules

- C++20 and C# with nullable on; follow `.clang-format` and `.editorconfig`; English in code and comments.
- Game version: one game and LeviLamina per build. `xmake.lua` `add_requires("levilamina <v>")`,
  `BuildInfo.LeviLaminaVersion` and `BuildInfo.GameVersion` (`1.` + LeviLamina's first two parts) in
  `app/BeDD/Core/BuildInfo.cs` must agree (CI checks).
- Before hooking a game or LeviLamina symbol, check the SDK headers under
  `%LOCALAPPDATA%\.xmake\packages\l\levilamina\<version>\<hash>\include`. Call only `MCAPI`, `MCFOLD` and `MCTAPI`
  functions.
- Protocol: change `spec/bedd.asyncapi.yaml` in the same pull request as any op change; keep `ping`.`protocol`
  (`kProtocol`, `BuildInfo.Protocol`) and the spec in agreement. A new op: one `else if` in `handle()`, listed in
  `ping`.`modes` and, if it only reads or hands its work to game threads, in `concurrentOp()` (else it runs under
  the input lock). Its no-argument form is read-only; behind a config key set `false` it answers
  `{"ok":false,"code":"disabled","key":"<key>"}`.
- Per-connection state: tag it with `currentClient()` (connection id from 1, never reused; 0 on the game thread)
  and release it from `clientGone()` in `src/Pipe.cpp` (one line, like `releaseGameButtonsOf`) or register
  `onClientGone(fn)` in `enable()`. Both run on the departing connection's thread after its keys and buttons are
  released, with input ops serialized, not at shutdown. Release only what has no end of its own: `key`/`mouse`
  `down` holds and an open-ended `button` `down`; `click`, `holdTicks`/`releaseOn` holds and the `input` override
  run to their end.
- Tap: `src/TapSink.h` stays byte-identical across publishers. Each publisher installs with
  `prev = gEmit.exchange(&own)`, forwards every frame to `prev`, and on removal restores `prev` only if `gEmit` is
  still its own (compare-exchange). Hooks only copy bytes; the writer thread sends.
- bedboo's packet-ownership table (`lib/bedd/authority.js`) and the WebSocket reference's "Who sends what" stay
  identical.
- CLI (`app/BeDD/Cli/Command.cs` has the details), after LeviLauncher's:
  - `BeDD.exe <noun> <verb> [arguments] [flags]`, or one word (`launch`, `dev`, `version`; `ps`/`stop` alias
    `process list|stop`). Nouns: `instance`, `mod`, `world`, `server`, `manager`, `process`.
  - Each command registers itself from a `[ModuleInitializer]` in its own file and touches no other file.
  - stdout carries only the result. `--json`: one document (array for lists, else object; camelCase, RFC 3339
    times, sizes in bytes). `-q`: identifiers only. Notices, warnings, `-v` go to stderr.
  - Failures throw `CliError` with an `ExitCode` (0 ok, 1 failure, 2 usage, 4 not found, 5 conflict, 10 declined)
    and a stable code (`ERR_...`); the dispatcher prints `error: <message>` and `hint: <runnable command>`, or
    `{"error":{code,message,exit,hint}}` with `--json`. Messages: plain lower-case English, no final period.
  - Names match exactly, ignoring case, never by prefix.
  - JSON is AOT-safe: `JsonNode` or a source-generated `JsonSerializerContext` in the file that uses it; no
    reflection.
- Instances: never write into a LeviLauncher instance. Find them with `LeviLauncher.exe instance list --json`
  (`BEDD_LEVILAUNCHER`), clone under `%LOCALAPPDATA%\BeDD\instances\<gameVersion>-<slot>` (hardlinks; copies across
  volumes). The clone's exe keeps the name `Minecraft.Windows.exe` and its data stays in `<clone>\Minecraft Bedrock`
  (LeviLauncher's loader resolves everything relative to the exe's folder).

### Pull requests

1. Fork, branch from `main`; one focused change per pull request; describe what it does and how it was tested.
2. Commit messages in the imperative mood ("Add wheel tilt").
3. User-visible changes go into [CHANGELOG.md](CHANGELOG.md) under *Unreleased*.

### Docs and showcase

- `docs/` is the site <https://docs.levimc.org/BeDD/> (levimc-kit, built by `.github/workflows/docs.yml`).
  `docs/public/showcase/` holds its gallery and the README pictures.
- Docs are minimal, unambiguous and complete: every sentence carries a fact, each fact lives on one page and others
  link to it.
- To drive a game, recommend bedboo first (or its ops), then `ui`, then simulated input (`key`, `text`, `mouse`,
  `raw`, `screenshot`) as the last resort.
- `tools/showcase/` (Node 24) reproduces the desktop video: `record.mjs` prepares three clones, restores
  `tools/showcase/fixtures/Showcase.zip`, starts the games in a manager window, runs `demo.mjs` off screen without
  the focus and records the manager window and the script's console with ffmpeg 8+ `gfxcapture` (each window's own
  picture, whatever covers it); `encode.mjs` puts a take (manager left, terminal right) into the MP4, WebP and
  poster files. Nothing moves in front of the user or takes the focus.

### Releasing

1. Set the version in `app/BeDD/BeDD.csproj` `<Version>` and the spec's `info.version` (they track the release).
   Move the *Unreleased*
   entries of `CHANGELOG.md` to `## [<version>]` with the date, and add its compare link at the bottom.
2. Check `BuildInfo` against `xmake.lua` (above) and that `ping`.`protocol` and the spec's version agree.
3. Tag the commit `v<version>` and push the tag. `release.yml` checks that the csproj and both mod manifests carry
   the tag's version and that CHANGELOG has its section (the release notes), builds both mod targets and
   `BeDD.exe`, and attaches `bedd-windows-x64-setup.exe`, `bedd-windows-x64.msi` (per user,
   `%LOCALAPPDATA%\Programs\BeDD`) and `bedd-windows-x64.zip`, all with the layout `BeDD/BeDD.exe`,
   `BeDD/mod/BeDD/`, `BeDD/mod/BeDD-server/`, `BeDD/spec/`. Keep it: `BeDD.exe` installs the modules from the
   `mod\` folder next to it.

## License

Contributions are licensed under [LGPL-3.0-or-later](COPYING.LESSER), the license of this project.
