# LeviHeadlessInput

Drive a LeviLamina Minecraft Bedrock **client** from scripts **without touching your mouse,
keyboard or foreground window**. A test-only client mod plus a PowerShell client, made for the
LeviLauncher e2e harnesses (BEI, BeetrootSkin, AlleviatedBackpacks), which used to send real
`SendInput` events and needed the game in the foreground: nobody could use the PC during a run.

Targets Minecraft **1.26.51.01** with LeviLamina **26.51.7** (client), like the projects using it.

## How it works

```
 harness (pwsh)                          Minecraft.Windows.exe + LeviHeadlessInput.dll
 ───────────────                         ─────────────────────────────────────────────
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

1. **Synthetic input goes through the game's own window procedure.** The mod subclasses the game
   window. Every request becomes ordinary window messages (`WM_KEYDOWN`, `WM_CHAR`,
   `WM_MOUSEMOVE`, `WM_LBUTTONDOWN`, `WM_MOUSEWHEEL`, or `WM_INPUT` for raw mouse), dispatched on
   the game thread in order. The game handles them exactly like real input
   (`MainGameCore::WndProc_PC::onKeyboardAction` → `HIDControllerGameCoreDesktop::onKeyDown`,
   `MouseDevice::feed`, ...), so UI hover, clicks, chat, key mappings all work. The reply is sent
   only after the game thread processed the input, so scripts are deterministic.
2. **The game never loses focus.** In background mode the focus-loss messages are swallowed, so the
   game does not pause, open the pause menu or release its keys when it is not the foreground
   window. At start-up the window goes to the bottom of the Z order and the foreground is handed back
   to the window you were using.
3. **Your real input never reaches the game.** Real client-area input messages are swallowed, and
   user32 calls through which the game reads or grabs the devices are hooked in-process to see a
   *virtual* state instead: `ClipCursor` (no-op: your cursor is never confined), `SetCursorPos`
   (moves the virtual cursor only), `GetCursorPos`/`GetPhysicalCursorPos`, `GetAsyncKeyState`,
   `GetKeyState`, `GetKeyboardState`, `GetForegroundWindow`/`GetActiveWindow`/`GetFocus` (report the
   game window), `SetForegroundWindow` (no-op for the game window), `GetRawInputData` (resolves the
   synthetic raw-input handles), `GetRawInputBuffer` (drops real buffered raw input).
4. **Screenshots** use `PrintWindow(PW_RENDERFULLCONTENT)`, which works while the window is covered.

Escape hatch: **Ctrl+Shift+F12** on the real keyboard while the game window has the focus turns
background mode off (real input works again), as does `Set-HeadlessBackground $false`.

## Build

Same toolchain as BeetrootSkin (xmake, MSVC build tools, LLVM `clang-cl`):

```powershell
$env:Path="C:\Program Files\LLVM\bin;"+[Environment]::GetEnvironmentVariable('Path','Machine')+';'+[Environment]::GetEnvironmentVariable('Path','User')
xmake f -a x64 -m release -p windows --target_type=client -y
xmake -y        # -> bin\LeviHeadlessInput\{LeviHeadlessInput.dll, manifest.json, HeadlessInput.psm1, harness-shim.ps1}
```

## Use it from a harness

`bin\LeviHeadlessInput\harness-shim.ps1` is a drop-in for the LeviLauncher harnesses. In `run.ps1`
(or wherever `lib.ps1` is dot-sourced):

```powershell
. "$PSScriptRoot/lib.ps1"
$headlessDir = if ($env:LEVI_HEADLESS_INPUT_DIR) { $env:LEVI_HEADLESS_INPUT_DIR } else { Join-Path $env:USERPROFILE 'Desktop\LeviHeadlessInput\bin\LeviHeadlessInput' }
if (-not $Foreground) {
    if (-not (Test-Path "$headlessDir\harness-shim.ps1")) { throw "LeviHeadlessInput not built ($headlessDir); build it or pass -Foreground" }
    . "$headlessDir\harness-shim.ps1"          # replaces Click/Press-Key/Focus-Game/... (same parameters)
    Register-HeadlessNative ([BsE2E.Native])   # routes [BsE2E.Native]::Key/MouseButton/... too (see below)
}
...
# after deploying your mod and moving the other mods aside:
if ($script:Headless) { Install-HeadlessInputMod -ModsDir $script:ModsDir }
...
finally {
    # after the game is closed:
    if ($script:Headless) { Uninstall-HeadlessInputMod -ModsDir $script:ModsDir }
}
```

Also add `'LeviHeadlessInput'` to the mods your "move other mods aside" step keeps (it is
installed fresh every run and removed afterwards; a leftover must never be parked and later
restored, because a normally started game with it would ignore your real mouse and keyboard).

Scenarios that call the harness's C# class directly (`[BsE2E.Native]::Key(0x10, $false)` to hold
Shift, `::MouseButton(1, $false)` to hold right-click, ...) keep working once the class has
redirect fields that `Register-HeadlessNative` fills:

```csharp
public static Action<ushort, bool> KeyRedirect;          // (vk, up)
public static Action<int, bool>    MouseButtonRedirect;  // (0 left / 1 right / 2 middle, up)
public static Action<int>          WheelRedirect;        // (delta, 120 per notch)
public static Action<char>         UnicodeRedirect;
public static Action<int, int>     MoveRelativeRedirect; // raw mouse (camera)
public static void Key(ushort vk, bool up) { if (KeyRedirect != null) { KeyRedirect(vk, up); return; } /* SendInput as before */ }
// ...same one-line guard at the top of MouseButton, Wheel, Unicode, MoveRelative
```

The shim replaces `Focus-Game` (no-op), `Assert-GameForeground` (checks the pipe instead),
`Set-GameWindowSize` (sized by the mod, never activated), `Move-Mouse`, `Click`, `Scroll`,
`Press-Key`, `Type-Text`, `Get-ClientBitmap` (`-FromScreen` ignored) and wraps `Start-Game`
(remembers your foreground window so the mod can hand it back). Coordinates are unchanged:
client pixels, or fractions of the client area with `-Relative`.

The game lock stays: there is still one game instance per machine, so runs still queue. They
just do not take the PC away from you any more.

## PowerShell client (`HeadlessInput.psm1`)

```powershell
Import-Module .\bin\LeviHeadlessInput\HeadlessInput.psm1
Connect-HeadlessInput                                   # waits for the pipe and the game window
Send-HeadlessKey E                                      # press; -Action down|up; -Vk 0x10
Send-HeadlessText '/give @s apple 3'                    # WM_CHAR, no key presses
Send-HeadlessMouse click -RX 0.5 -RY 0.474              # move|down|up|click|wheel; -X -Y pixels; -Button right
Send-HeadlessRaw move -Dx 200 -Dy 0                     # raw mouse (camera); click|down|up|wheel
Save-HeadlessScreenshot C:\temp\shot.png
Get-HeadlessState                                       # counters, hooked-API calls, real foreground?
Set-HeadlessBackground $false                           # hand the game back to the real devices
```

## Protocol

`\\.\pipe\LeviHeadlessInput` (local only), UTF-8, one JSON object per line each way. An `id` is
echoed. Replies are `{"ok":true,...}` or `{"ok":false,"error":"..."}`. Input ops reply after the
game thread has dispatched the input (`timeoutMs`, default 5000).

| op | fields | |
|---|---|---|
| `ping` | | `protocol`, `pid`, `hwnd`, `background`, `client{w,h}`, `cursor{x,y}`, `realForeground` |
| `state` | | `ping` fields + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api}` |
| `key` | `key` (name) or `vk`; `action` press\|down\|up; `holdMs` | names: letters, digits, ESC, ENTER, BACK, TAB, SPACE, SHIFT, CTRL, ALT, LEFT/UP/RIGHT/DOWN, HOME, END, PGUP, PGDN, INSERT, DELETE, F1..F24, SLASH, GRAVE, MINUS, EQUALS |
| `text` | `text`; `delayMs` | one `WM_CHAR` per UTF-16 unit |
| `mouse` | `action` move\|down\|up\|click\|wheel; `x`,`y` (client px) or `rx`,`ry` (fractions); `button` left\|right\|middle; `notches`; `holdMs` | no point = the virtual cursor |
| `raw` | `action` move\|down\|up\|click\|wheel; `dx`,`dy`; `button`; `notches` | synthetic `WM_INPUT` (relative) |
| `release` | | releases every held key/button |
| `focus` | | re-sends the "focus gained" messages |
| `window` | `clientWidth`, `clientHeight`, `placement` back\|offscreen\|none | never activates the window |
| `background` | `on` | background mode on/off |
| `flush` | | waits until everything posted was dispatched |

## Configuration

`mods\LeviHeadlessInput\config\config.json` (written by `Install-HeadlessInputMod`):

```json
{ "background": true, "placement": "back", "clientWidth": 1600, "clientHeight": 900, "trace": false }
```

Without the file the mod is passive (background off; the pipe still works). `trace` logs message
and API-hook statistics every 5 s plus every key/mouse event that reaches the game's input layer,
marked `synthetic` or `real`. The environment variable `LEVI_HEADLESS_RETURN_FOCUS` (a window
handle) tells the mod which window gets the foreground back.

## Developing

`tools\dev.ps1 [-Trace]` takes the shared e2e lock, deploys the mod, launches the game and logs
every foreground and cursor-clip change to `tools\out\desktop-watch.log` until the game exits or
`tools\out\dev.stop` exists; then it closes the game, removes the mod and releases the lock.
