# Contributing to Be.D.D.

Thanks for helping! Bug reports, ideas and pull requests are all welcome.

## Reporting bugs and requesting features

Use the [issue templates](https://github.com/LiteLDev/BeDD/issues/new/choose). For bugs, include:

- the Be.D.D., LeviLamina and Minecraft versions;
- what the script sent (the JSON request or the PowerShell call) and what the game did instead;
- `mods/BeDD/config/config.json`;
- the relevant part of `logs/latest.log`, ideally with `"trace": true` (it shows which input reached the game and whether it was synthetic or real).

## Development setup

See [Development](#development) below. The code is small:

| File | |
|---|---|
| `src/BeDD.h` | shared state and declarations of the parts below |
| `src/Mod.cpp` | entry point, config, virtual key/cursor state |
| `src/Window.cpp` | window discovery, subclassing, synthetic event dispatch, focus shield, placement |
| `src/ApiHooks.cpp` | user32 hooks (cursor, key state, foreground, activation, raw input) |
| `src/GameInputHooks.cpp` | GameInput v2 hooks (virtual mouse, keyboard readings) |
| `src/Audio.cpp` | FMOD hooks: the game's master channel group is muted in background mode (`mute`) |
| `src/NetHooks.cpp` | loopback-only network in background mode (`loopbackOnly`: no firewall prompt, no queries to servers off the PC) |
| `src/Pipe.cpp` | control pipe and protocol |
| `src/Manager.cpp` | link to the session's manager window |
| `src/Trace.cpp` | trace-mode input logging |
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
[CHANGELOG.md](CHANGELOG.md) under *Unreleased* for user-visible changes.

## Development

Material for people working on Be.D.D. itself, or talking to its pipe from something
other than `BeDD.psm1`. Script authors find everything they need in the [README](README.md).

### Building

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
first on a new PC, see the README), launches it through LeviLauncher and logs every
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
 test script (pwsh)                      Minecraft.Windows.exe + BeDD.dll
 ──────────────────                      ─────────────────────────────────────────────
 Send-BeDDKey E ──JSON line──▶ \\.\pipe\BeDD
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
   reads virtual buttons, relative position (camera), wheel and absolute position instead. GameInput
   has no reading for a mouse until the device reports input, so on an untouched PC the game's
   `GetCurrentReading` gets a virtual reading of that device from the mod.
3. **The game never takes the foreground or loses focus.** Focus-loss messages are swallowed, so
   the game does not pause or release its keys. `Start-BeDDGame.ps1` starts the exe with
   `SW_SHOWNOACTIVATE`; the hooked `ShowWindow`/`SetWindowPos`/`SetActiveWindow`/
   `BringWindowToTop`/`SetForegroundWindow` never activate the game's windows, and the window is
   kept at the bottom of the Z order. In background mode the user32 hooks are installed in `load()`
   already: the game creates and shows its window before `enable()`. The "focus gained" messages
   the mod sends skip the default `WM_ACTIVATE` handling (hooked `DefWindowProcW`), which would set
   the focus and so activate the window: a game started from the user's foreground app (a terminal,
   an agent) is allowed to take the foreground.
4. **Your real input never reaches the game.** Real client-area input messages are swallowed, and
   the user32 calls through which the game reads or grabs the devices see a *virtual* state:
   `ClipCursor` (no-op: your cursor is never confined), `SetCursorPos` (moves the virtual cursor
   only), `GetCursorPos`/`GetPhysicalCursorPos`, `GetAsyncKeyState`, `GetKeyState`,
   `GetKeyboardState`, `GetForegroundWindow`/`GetActiveWindow`/`GetFocus` (report the game window),
   `GetRawInputData`/`GetRawInputBuffer` (real raw input is dropped).
5. **Screenshots** use `PrintWindow(PW_RENDERFULLCONTENT)`, which works while the window is covered.
6. **The manager window** (`client/BeDDManager.ps1`, WinForms in C# compiled by `Add-Type`, one
   per logon session) shows every headless game through a DWM thumbnail, which stays live while
   the game window is off screen. `Start-BeDDGame.ps1` starts it through WMI
   (`Win32_Process.Create` inside `conhost --headless`), so it is not part of the caller's process
   tree or job and survives the agent or harness that happened to start it. The mod links to it in
   `load()`; a linked game creates its window as a tool window off screen (hooked
   `CreateWindowExW`: the game does not pump messages before `enable()`, so the window cannot be
   changed from outside its thread until then) and keeps it there (`SetWindowPos` moves are
   dropped, a watcher puts it back). The manager puts each linked game into a kill-on-close job of
   its own (once linked, so the start-up is left alone; a game that never links joins after a
   minute): closing or killing the manager closes the games.

The environment variable `BEDD_RETURN_FOCUS` (a window handle, set by
`Start-BeDDGame.ps1` and the shim's `Start-Game`) tells the mod which window gets the foreground
back if the game still activates itself while starting.

### Pipe protocol

`\\.\pipe\<pipeName>` (local only, default `BeDD`), UTF-8, one JSON object per line
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
| `stack` | `buf` (base64 item_stack_request); `view`; `timeoutMs` | item stack requests sent by the local client (`allowStack`, see below) |

`ping` and `state` also report `managed` (linked to the manager window) and `takenOver`.

#### `stack`: item stack requests through the client (`allowStack`)

Off by default. While `allowStack` is false every form answers
`{"ok":false,"code":"disabled","key":"allowStack","enabled":false}`.

| Form | Reply |
|---|---|
| `{op:"stack"}` (probe) | `{ok, enabled:true, via:"raw", stats:{sent, answered, rejected, failed, resyncs}}` |
| `{op:"stack", buf, timeoutMs=2000}` | `{ok, via:"raw", requestIds:[client ids], map:{"<caller id>": client id}}` |
| `{op:"stack", view:true}` | `{ok, inventory:[...], ui:[...], open?:{containerId, type, items:[...]}}`: what the client shows, each `{slot, item, count, stackId}` |

- `buf` is the base64 of one whole item_stack_request packet (id 147), varint header plus body, as
  bedrock-protocol serializes it. Any other packet id, and bytes the game does not read, are `bad_args`.
- On the next client tick the packet is rebuilt with `MinecraftPackets::createPacket` and
  `readNoHeader`, and the local player sends it.
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
  inventory, a chest) must already be open in the game: open it with `key E` or by using the block.
- Security: while `allowStack` is on, anything that can open the pipe (the current user, local only) can
  move, craft, drop and destroy items of the signed-in player on whatever server the client is on.
  Keep it off unless a harness needs it.

Authority table (bedd transport): `item_stack_request` is **translated via `stack`** (it used to be unsupported).
bedboo sends it through this op when the probe answers `ok`, and otherwise rejects it with
`ERR_BEDD_UNSUPPORTED`.

### Manager pipe

`\\.\pipe\BeDD-Manager-<session id>` (current user only), served by
`BeDDManager.ps1`, JSON lines like the control pipe.

- **Clients** (one request per connection): `ping` (`games`; `ok` is false while it closes),
  `adopt` `{pid, instance, label}` (`Start-BeDDGame.ps1` right after starting a game), `list`
  (`games[]`: `pid`, `instance`, `label`, `pipe`, `hwnd`, `linked`, `background`, `managed`,
  `takenOver`, `job`, `thumbnail`), `game` `{pid, action: show|hide|close}`, `shutdown`.
- **Games** (the mod): the first line is `hello` `{protocol, pid, hwnd, pipe, instance, label,
  background}` (`label` from `BEDD_LABEL`), answered with `{"ok":true}` (a manager that
  is closing hangs up instead; the game is linked only once welcomed). The connection stays open
  and the manager sends requests on it: every control-pipe op (it polls `state` every second), plus `show` (take over:
  background off, a normal window on screen and in front; the manager calls
  `AllowSetForegroundWindow` first), `hide` (hand back) and `close` (`WM_CLOSE` to the game window).

## License

By contributing you agree that your contributions are licensed under the [LGPL-3.0-or-later](COPYING.LESSER), the license of this project.
