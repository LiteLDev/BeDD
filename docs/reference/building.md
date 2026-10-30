# Building and contributing

## Building

You need Windows x64, [xmake](https://xmake.io), Visual Studio Build Tools with MSVC 14.4x or newer, and LLVM (`clang-cl`).
`tools/build.ps1` finds them, installs what is missing with winget, and builds:

```powershell
pwsh -NoProfile -File tools/build.ps1   # [-Mode debug]
```

By hand:

```powershell
xmake f -a x64 -m release -p windows --target_type=client -y
xmake -y        # -> bin\BeDD\ (DLL, manifest and everything in client\)
```

The mod packer reads the version from `git describe`, so the repository needs at least one commit.

`tools\dev.ps1 [-Trace]` is a playground: it takes the shared e2e lock
(`%APPDATA%\levilauncher.exe\bei-e2e.lock`), deploys the mod into the instance BeDD finds (set up
first on a new PC, see [Setting up a new PC](/guide/new-pc)), starts the manager window unless it runs,
launches the game through LeviLauncher (it shows up in the manager window) and logs every
foreground and cursor-clip change to `tools\out\desktop-watch.log` until the game exits or
`tools\out\dev.stop` exists (`tools\out\dev.restart` redeploys and relaunches without giving up the
lock). It always closes the game, removes the mod and releases the lock.

## Documentation and showcase media

`docs/` is the documentation site (<https://docs.levimc.org/BeDD/>, built with levimc-kit by
`.github/workflows/docs.yml`; `cd docs; npm ci; npm run build`). `docs/public/showcase/` holds its
gallery and the README pictures.
`tools/showcase/record.ps1 [-KeepOpen]` reproduces the desktop video: under the shared e2e lock
(plus the `hi<N>-e2e.lock` of each clone) it prepares three clone instances (`-hi2`..`-hi4`, other
mods moved aside), restores the `tools/showcase/fixtures/Showcase.zip` world, starts the games in
a manager window of its own (`BEDD_MANAGER_PIPE`, so other runs' games stay out of the take),
enters the world, runs `demo.ps1` in a console window off the screen and without the focus, and
records the manager window and that console with ffmpeg's `gfxcapture` (Windows.Graphics.Capture:
each window's own picture, whatever covers it) to `tools/showcase/out/take-<n>-manager.mkv` and
`take-<n>-terminal.mkv`. Nothing is moved in front of you and nothing takes the focus. With
`-KeepOpen`, creating `tools/showcase/out/take` starts another take and `tools/showcase/out/stop`
ends the session. `tools/showcase/encode.ps1` puts a take together, the manager on the left and the
terminal on the right, into the MP4, WebP and poster files.

## Source layout

| File | |
|---|---|
| `src/BeDD.h` | shared state and declarations of the parts below |
| `src/Mod.cpp` | entry point, config, virtual key/cursor state |
| `src/Window.cpp` | window discovery, subclassing, synthetic event dispatch, focus shield, window kept off screen |
| `src/ApiHooks.cpp` | user32 hooks (cursor, key state, foreground, activation, raw input) |
| `src/GameInputHooks.cpp` | GameInput v2 hooks (virtual mouse, keyboard readings) |
| `src/Audio.cpp` | FMOD hooks: the game's master channel group is muted (`mute`) |
| `src/NetHooks.cpp` | loopback-only network (`loopbackOnly`: no firewall prompt, no queries to servers off the PC) |
| `src/Pipe.cpp` | control pipe and protocol |
| `src/Manager.cpp` | link to the session's manager window |
| `src/Trace.cpp` | trace-mode input logging |
| `src/MoveInput.cpp` | `input` op: move override in the client's `ClientInputUpdateSystem` with a dead-man TTL (`gameHooks`) |
| `src/Send.cpp` | `send` op (`allowSend`): allowlisted serverbound packets, sent by the local player on the client thread |
| `src/GameState.cpp`, `src/Look.cpp` | `world` snapshot and absolute `look` (`gameHooks`) |
| `src/GameActions.cpp` | `slot`, `action` and tick-based `button` ops (`gameHooks`) |
| `src/Stack.cpp` | `stack` op (`allowStack`): item stack requests sent by the local client |
| `src/TapSink.h` | the frame type and sink chain every tap publisher shares (keep it byte-identical) |
| `src/Tap.cpp`, `src/Tap.h` | tap pipe: framing, markers, writer thread, the `tap` op |
| `src/NetTap.cpp` | tap capture on a remote server link, and the blob cache switch |
| `src/TapLoopback.cpp` | tap capture in singleplayer and LAN-host worlds (re-serialized packets) |
| `src/TapJournal.cpp` | the tap's join journal (compacted replay for a late reader) |
| `src/MemoryOperators.cpp` | routes `new`/`delete` through LeviLamina's allocator (required by LeviLamina) |
| `client/` | PowerShell client, harness shim, game start (`Start-BeDDGame.ps1`), manager window (`BeDDManager.ps1`), clone script and setup (`BeDDSetup.psm1`, `Initialize-BeDD.ps1`: finds the game instance and sets up what a new PC lacks), all shipped next to the DLL |

When you hook a game or LeviLamina symbol, check the SDK headers under
`%LOCALAPPDATA%\.xmake\packages\l\levilamina\<version>\<hash>\include` first. Only call `MCAPI`,
`MCFOLD` and `MCTAPI` functions.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep each pull request focused on one change. Describe what it does and how you tested it.
3. Follow the existing style: C++20, the provided `.clang-format` and `.editorconfig`, English in code and comments.
4. Test in game: `tools/dev.ps1 -Trace`, then drive the game through `BeDD.psm1` and check that
   the input works, the game never takes the foreground (`tools/out/desktop-watch.log`) and real input
   does not reach it.
5. CI must pass before a pull request is merged.

Commit messages use the imperative mood ("Add wheel tilt", not "Added …"). Update
[CHANGELOG.md](https://github.com/LiteLDev/BeDD/blob/main/CHANGELOG.md) under *Unreleased* for user-visible changes.

See also [CONTRIBUTING.md](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md). By contributing you agree that your contributions are licensed under the
[LGPL-3.0-or-later](https://github.com/LiteLDev/BeDD/blob/main/COPYING.LESSER), the license of this project.
