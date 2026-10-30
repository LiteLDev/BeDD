# How it works

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
2. **Mouse buttons, wheel and camera go through a virtual GameInput mouse.** Bedrock reads the mouse
   with the GDK **GameInput** API (`GameInputRedist.dll`, interface v2), straight from the device:
   window messages such as `WM_LBUTTONDOWN` are ignored, and without the mod your real mouse would
   still click and turn the camera in a background game. The mod hooks
   `IGameInputReading::GetMouseState` (and `GetKeyCount`/`GetKeyState`): the game reads virtual
   buttons, relative position (camera), wheel and absolute position instead. GameInput has no
   reading for a mouse until the device reports input, so on an untouched PC the game's
   `GetCurrentReading` gets a virtual reading of that device from the mod.
3. **The game never takes the foreground or loses focus.** Focus-loss messages are swallowed, so the
   game does not pause or release its keys. `Start-BeDDGame.ps1` starts the exe with
   `SW_SHOWNOACTIVATE`; the hooked `ShowWindow`/`SetWindowPos`/`SetActiveWindow`/
   `BringWindowToTop`/`SetForegroundWindow` never activate the game's windows, and the window is
   kept at the bottom of the Z order. The user32 hooks are installed in `load()` already: the game
   creates and shows its window before `enable()`. The "focus gained" messages the mod sends skip
   the default `WM_ACTIVATE` handling (hooked `DefWindowProcW`), which would set the focus and so
   activate the window: a game started from the user's foreground app (a terminal, an agent) is
   allowed to take the foreground.
4. **Your real input never reaches the game.** Real client-area input messages are swallowed, and
   the user32 calls through which the game reads or grabs the devices see a *virtual* state:
   `ClipCursor` (no-op: your cursor is never confined), `SetCursorPos` (moves the virtual cursor
   only), `GetCursorPos`/`GetPhysicalCursorPos`, `GetAsyncKeyState`, `GetKeyState`,
   `GetKeyboardState`, `GetForegroundWindow`/`GetActiveWindow`/`GetFocus` (report the game window),
   `GetRawInputData`/`GetRawInputBuffer` (real raw input is dropped).
5. **Screenshots** use `PrintWindow(PW_RENDERFULLCONTENT)`, which works while the window is covered.
6. **The manager window** (`client/BeDDManager.ps1`, WinForms in C# compiled by `Add-Type`, one per
   logon session) shows every game through a DWM thumbnail, which stays live while the game window
   is off screen, and is the only place a game shows up. `Start-BeDDGame.ps1` starts it through WMI
   (`Win32_Process.Create` inside `conhost --headless`), so it is not part of the caller's process
   tree or job and survives the agent or harness that happened to start it. The mod links to it in
   `load()`, and starts it when none runs (config `managerScript`: `pwsh -File <it> -Ensure`); a
   game no manager links within 90 s, or that loses the link for good, closes itself. The game
   always creates its window as a tool window off screen (hooked `CreateWindowExW`: the game does
   not pump messages before `enable()`, so the window cannot be changed from outside its thread
   until then) and keeps it there (`SetWindowPos` moves are dropped, a watcher puts it back). The
   manager puts each linked game into a kill-on-close job of its own (once linked, so the start-up
   is left alone; a game that never links joins after a minute): closing or killing the manager
   closes the games.

The environment variable `BEDD_RETURN_FOCUS` (a window handle, set by
`Start-BeDDGame.ps1` and the shim's `Start-Game`) tells the mod which window gets the foreground
back if the game still activates itself while starting.

