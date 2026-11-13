# How it works

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

1. **The transport is a WebSocket on loopback.** When the game loads the mod, it listens on
   `127.0.0.1` (config `wsPort`, default any free port), path `/v1`, and writes
   `%LOCALAPPDATA%\BeDD\run\<pid>.json` with the `url`, a random 32-byte `token` and the instance;
   the file is deleted at exit, and readers ignore one whose process is gone. A client sends the
   token as `Authorization: Bearer <token>` or `?token=<token>`; a request without it gets HTTP 401,
   and one with an `Origin` header (a web page) gets 403 unless config `allowOrigins` lists it. The
   server is a small RFC 6455 implementation on Winsock (SHA-1 from CNG): text frames carry the
   JSON requests and replies, binary frames the [tap](/reference/tap). Each connection has a thread of its
   own and is served in order; several connections may be open at once (`wsMaxClients`, default 8),
   and what one holds (keys, buttons, the `input` override) is released when it goes. The protocol
   is [`spec/bedd.asyncapi.yaml`](https://github.com/LiteLDev/BeDD/blob/main/spec/bedd.asyncapi.yaml),
   described on the [WebSocket reference](/reference/websocket).
2. **Three ways to drive the game, from meaning to pixels.** The *bedboo* ops (`world`, `look`,
   `input`, `slot`, `action`, `button`, `send`, `stack`, ...) hook the game's own systems and act
   on client ticks, whatever the key bindings. The *UI tree* op `ui` reads the JSON UI controls of
   the screens on the scene stack into an accessibility-style tree with refs, and acts on a ref
   through the virtual mouse and keys below. The *input* ops (`key`, `text`, `mouse`, `raw`,
   `screenshot`) are what a player's hands and eyes do; the points below are how they reach the
   game.
3. **Keyboard, text and pointer position go through the game's own window procedure.** The mod
   subclasses the game window. Keys become `WM_KEYDOWN`/`WM_KEYUP`, text `WM_CHAR`, the pointer
   `WM_MOUSEMOVE` plus a virtual cursor (what the hooked `GetCursorPos` returns), dispatched on the
   game thread in order. The game handles them exactly like real input
   (`MainGameCore::WndProc_PC::onKeyboardAction` → `HIDControllerGameCoreDesktop::onKeyDown`), so
   chat, key mappings and UI hover work. The reply is sent only after the game thread processed
   the input, so scripts are deterministic.
4. **Mouse buttons, wheel and camera go through a virtual GameInput mouse.** Bedrock reads the mouse
   with the GDK **GameInput** API (`GameInputRedist.dll`, interface v2), straight from the device:
   window messages such as `WM_LBUTTONDOWN` are ignored, and without the mod your real mouse would
   still click and turn the camera in a background game. The mod hooks
   `IGameInputReading::GetMouseState` (and `GetKeyCount`/`GetKeyState`): the game reads virtual
   buttons, relative position (camera), wheel and absolute position instead. GameInput has no
   reading for a mouse until the device reports input, so on an untouched PC the game's
   `GetCurrentReading` gets a virtual reading of that device from the mod.
5. **The game never takes the foreground or loses focus.** Focus-loss messages are swallowed, so the
   game does not pause or release its keys. `BeDD.exe launch` starts the exe with
   `CreateProcessW` and `SW_SHOWNOACTIVATE`; the hooked `ShowWindow`/`SetWindowPos`/`SetActiveWindow`/
   `BringWindowToTop`/`SetForegroundWindow` never activate the game's windows, and the window is
   kept at the bottom of the Z order. The user32 hooks are installed in `load()` already: the game
   creates and shows its window before `enable()`. The "focus gained" messages the mod sends skip
   the default `WM_ACTIVATE` handling (hooked `DefWindowProcW`), which would set the focus and so
   activate the window: a game started from the user's foreground app (a terminal, an agent) is
   allowed to take the foreground.
6. **Your real input never reaches the game.** Real client-area input messages are swallowed, and
   the user32 calls through which the game reads or grabs the devices see a *virtual* state:
   `ClipCursor` (no-op: your cursor is never confined), `SetCursorPos` (moves the virtual cursor
   only), `GetCursorPos`/`GetPhysicalCursorPos`, `GetAsyncKeyState`, `GetKeyState`,
   `GetKeyboardState`, `GetForegroundWindow`/`GetActiveWindow`/`GetFocus` (report the game window),
   `GetRawInputData`/`GetRawInputBuffer` (real raw input is dropped).
7. **Screenshots** (`screenshot` op) use `PrintWindow(PW_RENDERFULLCONTENT)` on the game window,
   which works while the window is covered, and come back as base64 PNG or JPEG, scaled down to
   `maxWidth` (960 by default) to keep them cheap.
8. **Every game is a clone.** `BeDD.exe` reads the user's LeviLauncher instance
   (`LeviLauncher.exe instance list --json`) and never writes into it. Each slot is a clone under
   `%LOCALAPPDATA%\BeDD\instances\<gameVersion>-<slot>` whose game files are hardlinks to the
   instance's (copies across volumes) and whose `mods\`, worlds and options are its own; a file
   that changes is replaced (deleted and written anew), never edited in place, so the instance's
   copy is never touched. LeviLauncher's loader in the patched `Minecraft.Windows.exe` resolves
   everything relative to the exe's folder, so a clone keeps its data in its own
   `Minecraft Bedrock` folder, as long as the exe keeps that name.
9. **The manager window** (`BeDD.exe` with no arguments, one per logon session) shows every game
   through a DWM thumbnail, which stays live while the game window is off screen, and is the only
   place a game shows up. `BeDD.exe launch` starts it when none runs, outside the caller's process
   tree, so it survives the agent or harness that happened to start it. It finds the games through
   their discovery files and attaches to each over its WebSocket (`{"op":"manager","action":"attach"}`).
   A game that no manager attaches within 30 s of creating its window, or whose attached connection
   drops, closes itself (config `manager`, default `true`; `false` keeps the game hidden and running
   without one, for CI). The game always creates its window as a tool window off screen (hooked
   `CreateWindowExW`: the game does not pump messages before `enable()`, so the window cannot be
   changed from outside its thread until then) and keeps it there (`SetWindowPos` moves are
   dropped, a watcher puts it back). Closing the manager window closes its games (`close` op).

The environment variable `BEDD_LABEL` names a game in the manager window, and `BEDD_RETURN_FOCUS`
(a window handle, set by `BeDD.exe launch`) tells the mod which window gets the foreground back if
the game still activates itself while starting.
