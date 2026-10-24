# LeviHeadlessInput

<img src="docs/logo.png" alt="" width="128" align="right">

English | [简体中文](README.zh-CN.md)

[![Release](https://img.shields.io/github/v/release/LiteLDev/LeviHeadlessInput)](https://github.com/LiteLDev/LeviHeadlessInput/releases)
[![License](https://img.shields.io/github/license/LiteLDev/LeviHeadlessInput)](COPYING.LESSER)

Run end-to-end tests of your [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mods
**in the background**. Your script presses keys, types and clicks inside Minecraft Bedrock, while
your own mouse, keyboard and windows stay yours, and several games can play at once.

<p align="center"><img src="docs/showcase/desktop.webp" alt="Three Minecraft windows react to a test script while the terminal in front keeps the focus" width="100%"></p>

> [!WARNING]
> This is a **testing tool**. While background mode is on, the game ignores your real mouse and
> keyboard. Do not leave it in a game you play (**Ctrl+Shift+F12** in the game window turns
> background mode off).

## Features

- **Keep using your PC.** The game never takes the foreground, the cursor or your keystrokes, and
  your clicks never land in it.
- **Several games at once.** Each game instance has its own pipe; a cheap clone of an instance
  takes a few seconds.
- **Plays like a player.** Keys, chat, key bindings, menus, hover tooltips, clicks and the camera
  all work as with a real mouse and keyboard.
- **Deterministic.** Every call returns once the game has handled the input, so no guessing sleeps.
- **Screenshots of covered windows.** The game can sit behind your other windows.
- **Drop-in for existing harnesses.** One file replaces the usual `Click`, `Press-Key`, `Focus-Game`
  helpers of LeviLauncher harnesses; scenarios stay as they are.

<p align="center"><img src="docs/showcase/how-it-feels.svg" alt="Your input goes to your apps; the script drives three background games over one pipe each" width="100%"></p>

## Install

Put it into the game instance your tests run, for example with
[lip](https://github.com/futrime/lip):

```powershell
lip install github.com/LiteLDev/LeviHeadlessInput#client
```

or unzip `leviheadlessinput-client-windows-x64.zip` from the
[releases](https://github.com/LiteLDev/LeviHeadlessInput/releases) into the instance's `mods\`
folder. Most harnesses copy it in at the start of a run and remove it afterwards
([see below](#plug-it-into-a-harness)).

Needs Minecraft **1.26.51.01** (GDK) with LeviLamina **26.51.7** (client), started through
LeviLauncher. Only the test PC's game needs it; servers and players do not.

## How to use

The PowerShell module ships next to the mod:

```powershell
Import-Module .\mods\LeviHeadlessInput\HeadlessInput.psm1
Connect-HeadlessInput                          # waits until the game is up
Send-HeadlessKey E                             # open the inventory
Send-HeadlessMouse click -RX 0.5 -RY 0.474     # click at a fraction of the window
Send-HeadlessKey T; Send-HeadlessText '/give @s apple 3'; Send-HeadlessKey ENTER
Save-HeadlessScreenshot C:\temp\shot.png       # works while the window is covered
```

| Command | What it does |
|---|---|
| `Connect-HeadlessInput [-PipeName p]` | wait for the game and pick which game the next calls go to |
| `Send-HeadlessKey E` | press a key; `-Action down`/`up` to hold, `-Vk 0x10` for any virtual key |
| `Send-HeadlessText '...'` | type text (no key presses, so `t` does not open chat); `-DelayMs` per character |
| `Send-HeadlessMouse click -RX 0.5 -RY 0.5` | `move`, `down`, `up`, `click`, `wheel` at `-X -Y` pixels or `-RX -RY` fractions; `-Button right` |
| `Send-HeadlessRaw move -Dx 200` | turn the camera in the world; `click -Button right` to use an item |
| `Clear-HeadlessInput` | release every key and button still held |
| `Save-HeadlessScreenshot path.png` / `Get-HeadlessBitmap` | the game's picture, even when covered |
| `Set-HeadlessWindow 1600 900` | resize the game window without bringing it forward |
| `Set-HeadlessBackground $false` | hand the game back to your real mouse and keyboard |
| `Get-HeadlessState` | is the game up, how much input arrived, did it ever get the foreground |

Key names: letters, digits, `ESC`, `ENTER`, `BACK`, `TAB`, `SPACE`, `SHIFT`, `CTRL`, `ALT`, arrows
(`LEFT`...), `HOME`, `END`, `PGUP`, `PGDN`, `INSERT`, `DELETE`, `F1`..`F24`, `SLASH`, `GRAVE`,
`MINUS`, `EQUALS`.

### Several games at once

```powershell
pwsh -NoProfile -File clone-instance.ps1 -Slot 2                  # versions\1.26.51.01-hi2, ~90 MB, seconds
& .\Start-HeadlessGame.ps1 -Instance 1.26.51.01-hi2                # starts it without bringing it forward
Connect-HeadlessInput -PipeName LeviHeadlessInput-1.26.51.01-hi2   # the pipeName in that instance's config
```

A clone has its own `mods\`, logs, worlds and options. A few rules keep parallel runs from
tripping over each other:

- Give every instance its own lock file, and find your game by its **instance folder**, never just
  by the name `Minecraft.Windows` (or you close someone else's game).
- Start games as `Minecraft.Windows.exe` (`Start-HeadlessGame.ps1` does): under another file name
  the game uses your normal Minecraft profile instead of the instance's.
- Make test worlds single-player with `Set-WorldSinglePlayer <worldDir>` (from the harness shim):
  games on one PC share an online sign-in, and multiplayer worlds opened at the same time fail with
  "An error has occurred" (code "Blaze").
- A run that joins a server online sets `$script:ExclusiveSignaling = $true` before `Start-Game`
  and gets the PC to itself while the other games close.

### Plug it into a harness

`harness-shim.ps1` replaces the usual helpers of LeviLauncher harnesses (`Focus-Game`,
`Assert-GameForeground`, `Set-GameWindowSize`, `Move-Mouse`, `Click`, `Scroll`, `Press-Key`,
`Type-Text`, `Get-ClientBitmap`, same parameters) and wraps `Start-Game`/`Stop-Game`:

```powershell
. "$PSScriptRoot/lib.ps1"
if (-not $Foreground) {
    . "$headlessDir\harness-shim.ps1"
    Register-HeadlessNative ([MyE2E.Native])   # optional, see below
}
# after deploying your mod and moving the other mods aside:
if ($script:Headless) { Install-HeadlessInputMod -ModsDir $script:ModsDir -PipeName 'LeviHeadlessInput-<instance>' }
# ... and in finally, once the game is closed:
if ($script:Headless) { Uninstall-HeadlessInputMod -ModsDir $script:ModsDir }
```

Start games with `Start-HeadlessGame.ps1` rather than the launcher, and keep `LeviHeadlessInput`
out of any "move other mods aside" step: it is installed fresh each run and removed afterwards (a
leftover copy would make a normally started game ignore your mouse and keyboard).

If your scenarios call the harness's own input class directly (`[MyE2E.Native]::Key(0x10, $false)`
and friends), give it these fields and a one-line guard at the top of each method;
`Register-HeadlessNative` fills them:

```csharp
public static Action<ushort, bool> KeyRedirect;          // (vk, up)
public static Action<int, bool>    MouseButtonRedirect;  // (0 left / 1 right / 2 middle, up)
public static Action<int>          WheelRedirect;        // (delta, 120 per notch)
public static Action<char>         UnicodeRedirect;
public static Action<int, int>     MoveRelativeRedirect; // camera
public static void Key(ushort vk, bool up) { if (KeyRedirect != null) { KeyRedirect(vk, up); return; } /* SendInput as before */ }
```

[BeetrootSkin](https://github.com/LiteLDev/BeetrootSkin/tree/main/tools/e2e) and
[AlleviatedBackpacks](https://github.com/LiteLDev/AlleviatedBackpacks/tree/main/tools/e2e) have
complete harnesses to copy from.

## Settings

`mods\LeviHeadlessInput\config\config.json`, usually written by the harness
(`Install-HeadlessInputMod` does it). Without it the mod stays passive: background mode off, the
pipe still answers.

```json
{ "background": true, "placement": "back", "clientWidth": 1600, "clientHeight": 900, "pipeName": "LeviHeadlessInput" }
```

| Key | |
|---|---|
| `background` | keep the game in the background and away from your real mouse and keyboard |
| `placement` | `back` (behind your windows), `offscreen` or `none` |
| `clientWidth`, `clientHeight` | game window size (0 = leave it), so clicks land in the same place every run |
| `pipeName` | one per game instance when several run at once |
| `loopbackOnly` | default `true`: no Windows Firewall prompt for each new instance (dedicated servers your harness starts still ask once; **Cancel** is fine) |
| `trace` | log every input that reaches the game to `logs\latest.log`, for bug reports |

## More

More videos and pictures: https://levimc.org/mods/github.com/LiteLDev/LeviHeadlessInput/?tab=gallery

Building it yourself, the pipe protocol or how it works inside? See [CONTRIBUTING.md](CONTRIBUTING.md#development).

## License

[LGPL-3.0-or-later](COPYING.LESSER). GameInput interface layouts follow Microsoft's public
`GameInput.h` (v2); no Microsoft code is included.
