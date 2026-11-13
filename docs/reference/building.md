# Building and contributing

## Building

You need Windows x64, [xmake](https://xmake.io), Visual Studio Build Tools (2022 or 2026) with the
C++ workload (MSVC 14.4x or newer), LLVM (`clang-cl`) for the mod, the .NET 10 SDK for `BeDD.exe`,
and Node.js 24 or newer for the documentation site. Install what is missing once with winget (LLVM
and the build tools ask for administrator rights), then open a new terminal so `PATH` has them:

```powershell
winget install -e --id Xmake-io.Xmake
winget install -e --id LLVM.LLVM
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
#   or Visual Studio 2026: --id Microsoft.VisualStudio.BuildTools with the same --override
winget install -e --id Microsoft.DotNet.SDK.10
winget install -e --id OpenJS.NodeJS.LTS
```

**The mod** (`bin\BeDD\`: the DLL and its manifest). xmake finds the build tools by itself; LLVM
has to be on `PATH`:

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
xmake f -P . -y -m release -p windows --target_type=client   # -m debug for a debug build
xmake -P . -y
```

`-P .` builds the project in the current folder. Without it xmake looks upwards for a project and,
inside a git worktree of another checkout, builds that one instead. The mod packer reads the
version from `git describe`, so the repository needs at least one commit (and its tags: `git fetch
--tags` in a shallow clone).

**Server target** (the client+server mode): `--target_type=server` above builds the mod a Bedrock
Dedicated Server with LeviLamina runs, into `bin\BeDD-server\` (DLL and manifest, platform
`server`). It holds the control pipe, the loopback-only network and the server ops
(`src/ServerTimeScale.cpp`) and nothing of the client (`xmake.lua` lists its files; the rest of
`src/` needs the client's LeviLamina headers). Copy the folder into the server's `mods\`
(`mods\BeDD-server\`); it reads `mods\BeDD-server\config\config.json` and serves
`\\.\pipe\BeDD-Server`.

```powershell
git checkout -- xmake-requires.lock
xmake f -P . -y -m release -p windows --target_type=server
xmake -P . -y
git checkout -- xmake-requires.lock
```

`xmake-requires.lock` pins the packages of both targets, but xmake rewrites it with the configured
target's pins only: `git checkout -- xmake-requires.lock` after building and before you configure
the other target (CI does the same). To move a pin, configure each target, keep both targets'
entries, and commit the union.

**The app** (`BeDD.exe`, C# NativeAOT). The AOT linker finds the C++ build tools through
`vswhere.exe`, which lives in the Visual Studio Installer folder; put it on `PATH` for the publish:

```powershell
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
dotnet publish app/BeDD/BeDD.csproj -c Release -o bin\app   # -> bin\app\BeDD.exe
dotnet test app/BeDD.slnx                                   # unit tests (no vswhere needed)
```

**The dev loop.** `bin\app\BeDD.exe dev` is a playground for the mod you just built: it clones the
base instance for a dev slot (`instance clone`, slot 9 unless `--slot`), installs `bin\BeDD` into
the clone (`mod install`, or `--from <dir>`), launches the game in the background (`launch`, label
`dev` unless `--label`) and keeps running. Meanwhile it logs every change of the foreground window
and of the cursor clip rectangle to `tools\out\desktop-watch.log` (or `--watch-log <file>`): a line
ending in `MINECRAFT HAS THE FOREGROUND` means a game took the focus (dev also prints it as a
warning), and the clip has to stay `full`. That log is the evidence that the game does not take
the user's mouse or keyboard.

```powershell
bin\app\BeDD.exe dev                 # Ctrl+C closes the game and exits
bin\app\BeDD.exe dev --slot 3 --keep # leave the game running when dev exits
bin\app\BeDD.exe dev --watch-only    # only watch the desktop (games started some other way)
```

Ctrl+C, or creating `tools\out\dev.stop`, closes the game (unless `--keep`) and ends dev;
`tools\out\dev.restart` closes the game, installs the rebuilt mod again and relaunches it. Dev
also ends when the game exits. It only ever closes games started from its own clone, and one dev
loop at a time uses a slot (the lock is `%LOCALAPPDATA%\BeDD\locks\dev-<slot>.lock`, released when
dev exits, however it exits). `BeDD.exe` is a windowed program, so a shell may not wait for it:
in PowerShell pipe it (`bin\app\BeDD.exe dev | Out-Host`) to keep the prompt until dev ends, and
use `tools\out\dev.stop` to end it then.

## Documentation and showcase media

`docs/` is the documentation site (<https://docs.levimc.org/BeDD/>, built with levimc-kit by
`.github/workflows/docs.yml`; `cd docs; npm ci; npm run build`). `docs/public/showcase/` holds its
gallery and the README pictures.
`tools/showcase/record.mjs [--keep-open] [--dry-run]` (Node 24, no dependencies) reproduces the
desktop video: it makes three clones with `BeDD.exe instance clone`, installs the mod, restores the
`tools/showcase/fixtures/Showcase.zip` world, launches the games with `BeDD.exe launch` and drives
them over their WebSocket (bedboo ops first, `ui` for menus, raw input only where nothing semantic
exists), while ffmpeg records the manager window to `tools/showcase/out/take-<n>-manager.mkv` and the
script's own log to `take-<n>-terminal.mkv`. Nothing is moved in front of you and nothing takes the
focus. With `--keep-open`, creating `tools/showcase/out/take` starts another take and
`tools/showcase/out/stop` ends the session. `node tools/showcase/encode.mjs` puts a take together,
the manager on the left and the terminal on the right, into the MP4, WebP and poster files.

## Source layout

| File | |
|---|---|
| `src/BeDD.h` | shared state and declarations of the parts below |
| `src/Mod.cpp` | entry point, config, virtual key/cursor state |
| `src/Window.cpp` | window discovery, subclassing, synthetic event dispatch, focus shield, window kept off screen |
| `src/ApiHooks.cpp` | user32 hooks (cursor, key state, foreground, activation, raw input) |
| `src/GameInputHooks.cpp` | GameInput v2 hooks (virtual mouse, keyboard readings) |
| `src/Audio.cpp` | FMOD hooks: the game's master channel group is muted (`mute`) |
| `src/NetHooks.cpp` | loopback-only network (`loopbackOnly`: no firewall prompt, no queries to servers off the PC); in the server target too |
| `src/ServerTimeScale.cpp` | server target only: the `server`, `timescale` and `command` ops, config `timeScale`, LevelEvent 3007 to the players |
| `src/Pipe.cpp` | the ops: `handle()` dispatch, per-connection ownership and cleanup |
| `src/WsServer.cpp`, `src/Discovery.cpp` | the WebSocket server (127.0.0.1, token, `/v1`) and `run\<pid>.json` |
| `src/Screenshot.cpp` | `screenshot` op (PrintWindow + WIC) |
| `src/UiTree.cpp`, `src/UiJson.cpp`, `src/UiActions.cpp` | `ui` op: JSON UI tree, refs, click/setText/scroll |
| `src/UiOre.cpp` | `oreui` op: the Ore UI DOM through the game's V8 |
| `src/Manager.cpp` | the `manager` and `close` ops: kept hidden while the manager is attached |
| `src/Trace.cpp` | trace-mode input logging |
| `src/MoveInput.cpp` | `input` op: move override in the client's `ClientInputUpdateSystem` with a dead-man TTL (`gameHooks`) |
| `src/Send.cpp` | `send` op (`allowSend`): allowlisted serverbound packets, sent by the local player on the client thread |
| `src/GameState.cpp`, `src/Look.cpp` | `world` snapshot and absolute `look` (`gameHooks`) |
| `src/GameActions.cpp` | `slot`, `action` and tick-based `button` ops (`gameHooks`) |
| `src/Stack.cpp` | `stack` op (`allowStack`): item stack requests sent by the local client |
| `src/TapSink.h` | the frame type and sink chain every tap publisher shares (keep it byte-identical) |
| `src/Tap.cpp`, `src/Tap.h` | the tap: framing, markers, writer thread, the `tap` op and its WebSocket subscriber |
| `src/NetTap.cpp` | tap capture on a remote server link, and the blob cache switch |
| `src/TapLoopback.cpp` | tap capture in singleplayer and LAN-host worlds (re-serialized packets) |
| `src/TapJournal.cpp` | the tap's join journal (compacted replay for a late reader) |
| `src/MemoryOperators.cpp` | routes `new`/`delete` through LeviLamina's allocator (required by LeviLamina) |
| `app/` | `BeDD.exe` (C# NativeAOT): `Cli/` commands, `Core/` instances, launch, servers, discovery, WebSocket client, `Gui/` manager window; `BeDD.Tests/` |
| `tests/ws/` | protocol tests against a running game (`node --test tests/ws/`) |
| `spec/bedd.asyncapi.yaml` | the WebSocket API (AsyncAPI 3.0) |

When you hook a game or LeviLamina symbol, check the SDK headers under
`%LOCALAPPDATA%\.xmake\packages\l\levilamina\<version>\<hash>\include` first. Only call `MCAPI`,
`MCFOLD` and `MCTAPI` functions.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep each pull request focused on one change. Describe what it does and how you tested it.
3. Follow the existing style: C++20, the provided `.clang-format` and `.editorconfig`, English in code and comments.
4. Test in game: `bin\app\BeDD.exe dev`, then drive the game through `BeDD.exe` and check that
   the input works, the game never takes the foreground (`tools/out/desktop-watch.log`) and real input
   does not reach it.
5. CI must pass before a pull request is merged.

Commit messages use the imperative mood ("Add wheel tilt", not "Added …"). Update
[CHANGELOG.md](https://github.com/LiteLDev/BeDD/blob/main/CHANGELOG.md) under *Unreleased* for user-visible changes.

See also [CONTRIBUTING.md](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md). By contributing you agree that your contributions are licensed under the
[LGPL-3.0-or-later](https://github.com/LiteLDev/BeDD/blob/main/COPYING.LESSER), the license of this project.
