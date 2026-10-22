# LeviHeadlessInput

English | [简体中文](README.zh-CN.md)

Drive a [LeviLamina](https://github.com/LiteLDev/LeviLamina) Minecraft Bedrock **client** from
scripts **without touching your mouse, keyboard or foreground window**, and run **several game
instances at once**.

End-to-end tests of client mods usually send real input (`SendInput`) to a game in the foreground:
nobody can use the PC during a run, and only one game can be tested at a time. LeviHeadlessInput is
a test-only client mod with a PowerShell client. A script talks to the game over a named pipe; the
mod feeds the input into the game itself while the game stays in the background and ignores the
real devices.

> [!WARNING]
> This is a **testing tool**. While it is loaded with background mode on, the game ignores your
> real mouse and keyboard. Do not leave it installed in a game you play
> (**Ctrl+Shift+F12** in the game window turns background mode off).

Targets Minecraft **1.26.51.01** (GDK) with LeviLamina **26.51.7** (client).

## How it works

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

## Install

```powershell
lip install github.com/LiteLDev/LeviHeadlessInput     # into the game instance's mods\ folder
```

or unzip `leviheadlessinput-client-windows-x64.zip` from the
[releases](https://github.com/LiteLDev/LeviHeadlessInput/releases) into `mods\`. Test harnesses
usually deploy it themselves for each run (see [Use it from a harness](#use-it-from-a-harness)).

Without `mods\LeviHeadlessInput\config\config.json` the mod is passive (background mode off; the
pipe still works). A harness writes:

```json
{ "background": true, "placement": "back", "clientWidth": 1600, "clientHeight": 900, "trace": false, "pipeName": "LeviHeadlessInput" }
```

| Key | |
|---|---|
| `background` | shield the game from focus loss and the real devices, keep it out of the foreground |
| `placement` | `back` (bottom of the Z order), `offscreen` or `none` |
| `clientWidth`, `clientHeight` | client-area size applied once the window exists (0 = leave it) |
| `trace` | log message/API-hook statistics every 5 s and every key/mouse event that reaches the game's input layer |
| `pipeName` | `\\.\pipe\<pipeName>`; one per game instance when several run at once |

The environment variable `LEVI_HEADLESS_RETURN_FOCUS` (a window handle, set by
`Start-HeadlessGame.ps1`) tells the mod which window gets the foreground back if the game still
activates itself while starting.

## PowerShell client

`HeadlessInput.psm1` ships next to the mod:

```powershell
Import-Module .\mods\LeviHeadlessInput\HeadlessInput.psm1
Connect-HeadlessInput                                   # waits for the pipe and the game window
Send-HeadlessKey E                                      # press; -Action down|up; -Vk 0x10
Send-HeadlessText '/give @s apple 3'                    # WM_CHAR, no key presses
Send-HeadlessMouse click -RX 0.5 -RY 0.474              # move|down|up|click|wheel; -X -Y pixels; -Button right
Send-HeadlessRaw move -Dx 200 -Dy 0                     # camera (relative); down|up|click -Button right to use an item
Save-HeadlessScreenshot C:\temp\shot.png
Get-HeadlessState                                       # counters, hooked-API calls, real foreground?
Set-HeadlessBackground $false                           # hand the game back to the real devices
Set-HeadlessPipe LeviHeadlessInput-2                    # talk to another instance
```

## Several games at once

Because nothing needs the foreground, several game instances can run side by side, each driven
through its own pipe:

```powershell
pwsh -NoProfile -File clone-instance.ps1 -Slot 2                      # versions\1.26.51.01-hi2
& .\Start-HeadlessGame.ps1 -Instance 1.26.51.01-hi2                    # no launcher, no activation
Connect-HeadlessInput -PipeName LeviHeadlessInput-1.26.51.01-hi2       # pipeName from its config.json
```

`clone-instance.ps1` makes `versions\<base>-hi<N>` from a LeviLauncher instance with hard links
(game files) and junctions (`data`, `Installers`), and copies only the per-instance state
(`Minecraft Bedrock`: worlds, options, sign-in; `mods\LeviLamina`), so a clone costs ~90 MB and a
few seconds. Each clone has its own `mods\`, `logs\latest.log` and worlds. Bedrock does not stop a
second process. Rules for harnesses:

- Give every instance ("slot") its own lock file.
- Match game processes by **instance folder**, never by the name `Minecraft.Windows` alone: a
  harness that focuses, feeds or closes every `Minecraft.Windows` process breaks the other runs.
- Always start the exe as `Minecraft.Windows.exe`: LeviLauncher's data isolation keys on that file
  name. Under another name the game silently uses the default `%APPDATA%\Minecraft Bedrock`
  profile instead of the instance's data.
- Windows Firewall may ask once per new exe path (the game opens network sockets): **Cancel** is fine.

[BeetrootSkin](https://github.com/LiteLDev/BeetrootSkin/tree/main/tools/e2e) and
[AlleviatedBackpacks](https://github.com/LiteLDev/AlleviatedBackpacks/tree/main/tools/e2e) have
complete harnesses (`run.ps1 -Slot`, `Set-E2EInstance`).

## Use it from a harness

`harness-shim.ps1` is a drop-in for PowerShell harnesses with the usual helper names. Dot-source it
right after the harness's own `lib.ps1`, at script level:

```powershell
. "$PSScriptRoot/lib.ps1"
if (-not $Foreground) {
    . "$headlessDir\harness-shim.ps1"          # replaces Click/Press-Key/Focus-Game/... (same parameters)
    Register-HeadlessNative ([MyE2E.Native])   # routes [MyE2E.Native]::Key/MouseButton/... too (see below)
}
...
# after deploying your mod and moving the other mods aside:
if ($script:Headless) { Install-HeadlessInputMod -ModsDir $script:ModsDir -PipeName 'LeviHeadlessInput-<instance>' }
...
finally {
    # after the game is closed:
    if ($script:Headless) { Uninstall-HeadlessInputMod -ModsDir $script:ModsDir }
}
```

The shim replaces `Focus-Game` (no-op), `Assert-GameForeground` (checks the pipe instead),
`Set-GameWindowSize` (sized by the mod, never activated), `Move-Mouse`, `Click`, `Scroll`,
`Press-Key`, `Type-Text` and `Get-ClientBitmap` (`-FromScreen` ignored), and wraps `Start-Game`.
Coordinates are unchanged: client pixels, or fractions of the client area with `-Relative`. Start
headless games with `Start-HeadlessGame.ps1` rather than through the launcher.

Keep `'LeviHeadlessInput'` out of any "move other mods aside" step: it is installed fresh every run
and removed afterwards. A leftover must never be parked and restored later, because a normally
started game with it ignores the real mouse and keyboard.

Scenarios that call the harness's C# class directly (`[MyE2E.Native]::Key(0x10, $false)` to hold
Shift, `::MouseButton(1, $false)` to hold right-click, ...) keep working once the class has
redirect fields that `Register-HeadlessNative` fills:

```csharp
public static Action<ushort, bool> KeyRedirect;          // (vk, up)
public static Action<int, bool>    MouseButtonRedirect;  // (0 left / 1 right / 2 middle, up)
public static Action<int>          WheelRedirect;        // (delta, 120 per notch)
public static Action<char>         UnicodeRedirect;
public static Action<int, int>     MoveRelativeRedirect; // relative mouse (camera)
public static void Key(ushort vk, bool up) { if (KeyRedirect != null) { KeyRedirect(vk, up); return; } /* SendInput as before */ }
// ...same one-line guard at the top of MouseButton, Wheel, Unicode, MoveRelative
```

## Protocol

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

## Building

Windows x64, [xmake](https://xmake.io), Visual Studio Build Tools (MSVC 14.4x or newer) and LLVM
(`clang-cl`):

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

## License

[LGPL-3.0-or-later](COPYING.LESSER). GameInput interface layouts follow Microsoft's public
`GameInput.h` (v2); no Microsoft code is included.
