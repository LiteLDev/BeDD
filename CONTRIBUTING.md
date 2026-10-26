# Contributing to LeviHeadlessInput

Thanks for helping! Bug reports, ideas and pull requests are all welcome.

## Reporting bugs and requesting features

Use the [issue templates](https://github.com/LiteLDev/LeviHeadlessInput/issues/new/choose). For bugs, include:

- the LeviHeadlessInput, LeviLamina and Minecraft versions;
- what the script sent (the JSON request or the PowerShell call) and what the game did instead;
- `mods/LeviHeadlessInput/config/config.json`;
- the relevant part of `logs/latest.log`, ideally with `"trace": true` (it shows which input reached the game and whether it was synthetic or real).

## Development setup

See [Development](#development) below. The code is small:

| File | |
|---|---|
| `src/Headless.h` | shared state and declarations of the parts below |
| `src/Mod.cpp` | entry point, config, virtual key/cursor state |
| `src/Window.cpp` | window discovery, subclassing, synthetic event dispatch, focus shield, placement |
| `src/ApiHooks.cpp` | user32 hooks (cursor, key state, foreground, activation, raw input) |
| `src/GameInputHooks.cpp` | GameInput v2 hooks (virtual mouse, keyboard readings) |
| `src/NetHooks.cpp` | loopback-only sockets in background mode (`loopbackOnly`, no firewall prompt) |
| `src/Pipe.cpp` | control pipe and protocol |
| `src/Trace.cpp` | trace-mode input logging |
| `src/MemoryOperators.cpp` | routes `new`/`delete` through LeviLamina's allocator (required by LeviLamina) |
| `client/` | PowerShell client, harness shim, game start (`Start-HeadlessGame.ps1`) and clone scripts (shipped next to the DLL) |

When you hook a game or LeviLamina symbol, check the SDK headers under
`%LOCALAPPDATA%\.xmake\packages\l\levilamina\<version>\<hash>\include` first. Only call `MCAPI`,
`MCFOLD` and `MCTAPI` functions.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep each pull request focused on one change. Describe what it does and how you tested it.
3. Follow the existing style: C++20, the provided `.clang-format` and `.editorconfig`, English in code and comments.
4. Test in game: `tools/dev.ps1 -Trace`, then drive the game through `HeadlessInput.psm1` and check that
   the input works, the game never takes the foreground (`tools/out/desktop-watch.log`) and real input
   does not reach it.
5. CI must pass before a pull request is merged.

Commit messages use the imperative mood ("Add wheel tilt", not "Added …"). Update
[CHANGELOG.md](CHANGELOG.md) under *Unreleased* for user-visible changes.

## Development

Material for people working on LeviHeadlessInput itself, or talking to its pipe from something
other than `HeadlessInput.psm1`. Script authors find everything they need in the [README](README.md).

### Building

You need Windows x64, [xmake](https://xmake.io), Visual Studio Build Tools with MSVC 14.4x or newer, and LLVM (`clang-cl`).

```powershell
xmake f -a x64 -m release -p windows --target_type=client -y
xmake -y        # -> bin\LeviHeadlessInput\ (DLL, manifest, HeadlessInput.psm1, harness-shim.ps1, Start-HeadlessGame.ps1, clone-instance.ps1)
```

The mod packer reads the version from `git describe`, so the repository needs at least one commit.

`tools\dev.ps1 [-Trace]` is a playground: it takes the shared e2e lock
(`%APPDATA%\levilauncher.exe\bei-e2e.lock`), deploys the mod, launches the game and logs every
foreground and cursor-clip change to `tools\out\desktop-watch.log` until the game exits or
`tools\out\dev.stop` exists (`tools\out\dev.restart` redeploys and relaunches without giving up the
lock). It always closes the game, removes the mod and releases the lock.

### Showcase media

`docs/showcase/` holds the gallery shown on LeviMC Mods (`showcase.json`) and the README pictures.
`tools/showcase/record.ps1 [-KeepOpen]` reproduces the desktop video: under the shared e2e lock
(plus the `hi<N>-e2e.lock` of each clone) it prepares three clone instances (`-hi2`..`-hi4`, other
mods moved aside), restores the `tools/showcase/fixtures/Showcase.zip` world, starts the games in
the background, enters the world, lays the windows out in a 2x2 grid over a backdrop (`stage.ps1`)
with Windows Terminal running `demo.ps1` in the fourth cell, and records that region of the real
screen with ffmpeg's Desktop Duplication grabber to `tools/showcase/out/take-<n>.mkv`. With
`-KeepOpen`, creating `tools/showcase/out/take` starts another take and `tools/showcase/out/stop`
ends the session. `tools/showcase/encode.ps1` cuts a take into the MP4, WebP and poster files.

### How it works

```
 test script (pwsh)                      Minecraft.Windows.exe + LeviHeadlessInput.dll
 ──────────────────                      ─────────────────────────────────────────────
 Send-HeadlessKey E ──JSON line──▶ \\.\pipe\LeviHeadlessInput
                                          │ queue Event{WM_KEYDOWN 'E'}; PostMessage(hwnd, WM_LHI)
                                          ▼
                                   subclassed window procedure (game thread)
                                     WM_LHI → apply virtual key/cursor state
                                            → CallWindowProc(game's WndProc, WM_KEYDOWN, ...)
                                     real WM_KEY*/WM_MOUSE*/WM_INPUT/WM_POINTER* → swallowed
                                     WM_ACTIVATEAPP(0)/WM_KILLFOCUS/WM_ACTIVATE(inactive) → swallowed
 ◀──{"ok":true}── after the game thread dispatched it
```

1. **Keyboard, text and pointer position go through the game's own window procedure.** The mod
   subclasses the game window. Keys become `WM_KEYDOWN`/`WM_KEYUP`, text `WM_CHAR`, the pointer
   `WM_MOUSEMOVE` plus a virtual cursor (what the hooked `GetCursorPos` returns), dispatched on the
   game thread in order. The game handles them exactly like real input
   (`MainGameCore::WndProc_PC::onKeyboardAction` → `HIDControllerGameCoreDesktop::onKeyDown`), so
   chat, key mappings and UI hover work. The reply is sent only after the game thread processed
   the input, so scripts are deterministic.
2. **Mouse buttons, wheel and camera go through a virtual GameInput mouse.** Bedrock reads the
   mouse with the GDK **GameInput** API (`GameInputRedist.dll`, interface v2), straight from the
   device: window messages such as `WM_LBUTTONDOWN` are ignored, and without the mod your real
   mouse would still click and turn the camera in a background game. The mod hooks
   `IGameInputReading::GetMouseState` (and `GetKeyCount`/`GetKeyState`): in background mode the game
   reads virtual buttons, relative position (camera), wheel and absolute position instead.
3. **The game never takes the foreground or loses focus.** Focus-loss messages are swallowed, so
   the game does not pause or release its keys. `Start-HeadlessGame.ps1` starts the exe with
   `SW_SHOWNOACTIVATE`; the hooked `ShowWindow`/`SetWindowPos`/`SetActiveWindow`/
   `BringWindowToTop`/`SetForegroundWindow` never activate the game's windows, and the window is
   kept at the bottom of the Z order.
4. **Your real input never reaches the game.** Real client-area input messages are swallowed, and
   the user32 calls through which the game reads or grabs the devices see a *virtual* state:
   `ClipCursor` (no-op: your cursor is never confined), `SetCursorPos` (moves the virtual cursor
   only), `GetCursorPos`/`GetPhysicalCursorPos`, `GetAsyncKeyState`, `GetKeyState`,
   `GetKeyboardState`, `GetForegroundWindow`/`GetActiveWindow`/`GetFocus` (report the game window),
   `GetRawInputData`/`GetRawInputBuffer` (real raw input is dropped).
5. **Screenshots** use `PrintWindow(PW_RENDERFULLCONTENT)`, which works while the window is covered.

The environment variable `LEVI_HEADLESS_RETURN_FOCUS` (a window handle, set by
`Start-HeadlessGame.ps1` and the shim's `Start-Game`) tells the mod which window gets the foreground
back if the game still activates itself while starting.

### Pipe protocol

`\\.\pipe\<pipeName>` (local only, default `LeviHeadlessInput`), UTF-8, one JSON object per line
each way. An `id` is echoed. Replies are `{"ok":true,...}` or `{"ok":false,"error":"..."}`. Input
ops reply after the game thread has dispatched the input (`timeoutMs`, default 5000).

| op | fields | |
|---|---|---|
| `ping` | | `protocol`, `pid`, `hwnd`, `background`, `client{w,h}`, `cursor{x,y}`, `realForeground` |
| `state` | | `ping` fields + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput}` |
| `key` | `key` (name) or `vk`; `action` press\|down\|up; `holdMs` | names: letters, digits, ESC, ENTER, BACK, TAB, SPACE, SHIFT, CTRL, ALT, LEFT/UP/RIGHT/DOWN, HOME, END, PGUP, PGDN, INSERT, DELETE, F1..F24, SLASH, GRAVE, MINUS, EQUALS |
| `text` | `text`; `delayMs` | one `WM_CHAR` per UTF-16 unit |
| `mouse` | `action` move\|down\|up\|click\|wheel; `x`,`y` (client px) or `rx`,`ry` (fractions); `button` left\|right\|middle; `notches`; `holdMs` | moves the virtual cursor (if a point is given), then buttons/wheel on the virtual GameInput mouse |
| `raw` | `action` move\|down\|up\|click\|wheel; `dx`,`dy` (mouse counts); `steps`; `button`; `notches`; `holdMs` | virtual GameInput mouse: camera while the mouse is grabbed |
| `release` | | releases every held key/button |
| `focus` | | re-sends the "focus gained" messages |
| `window` | `clientWidth`, `clientHeight`, `placement` back\|offscreen\|none | never activates the window |
| `background` | `on` | background mode on/off |
| `flush` | | waits until everything posted was dispatched |

## License

By contributing you agree that your contributions are licensed under the [LGPL-3.0-or-later](COPYING.LESSER), the license of this project.
