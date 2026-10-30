# Be.D.D.

<img src="docs/public/logo.png" alt="" width="128" align="right">

English | [简体中文](README.zh-CN.md)

[![Release](https://img.shields.io/github/v/release/LiteLDev/BeDD)](https://github.com/LiteLDev/BeDD/releases)
[![License](https://img.shields.io/github/license/LiteLDev/BeDD)](COPYING.LESSER)
[![Docs](https://img.shields.io/badge/docs-docs.levimc.org%2FBeDD-blue)](https://docs.levimc.org/BeDD/)

Be.D.D. (Bedrock Deep Dive) is named after the H.D.D. of Zenless Zone Zero: a Proxy guides agents through the
Hollows from afar, your script guides Minecraft from the background.

Run end-to-end tests of your [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mods
**in the background**. Your script presses keys, types and clicks inside Minecraft Bedrock, while
your own mouse, keyboard and windows stay yours, and several games can play at once.

Be.D.D. is a testing toolkit, not a mod for players: a client-side LeviLamina module that your test
harness puts into a game instance for a run, the PowerShell client that drives it, and the scripts
that set up, clone and start the games. **Documentation: <https://docs.levimc.org/BeDD/>**

<p align="center"><img src="docs/public/showcase/desktop.webp" alt="Three headless Minecraft games react to a test script in the manager window" width="100%"></p>

> [!WARNING]
> This is a **testing tool**. A game with Be.D.D. in its `mods\` folder ignores your real mouse
> and keyboard and shows up only in the manager window. Do not leave it in a game you play.

## Features

- **Keep using your PC.** The game never takes the foreground, the cursor or your keystrokes, and
  your clicks never land in it.
- **Several games at once.** Each game instance has its own pipe; a cheap clone of an instance
  takes a few seconds.
- **One window for all of them.** The games of every test run on the PC show up live in one
  manager window instead of popping up on your desktop; closing it closes them all.
- **Plays like a player.** Keys, chat, key bindings, menus, hover tooltips, clicks and the camera
  all work as with a real mouse and keyboard.
- **Deterministic.** Every call returns once the game has handled the input, so no guessing sleeps.
- **Screenshots of covered windows.** The manager window can sit behind your other windows.
- **Drop-in for existing harnesses.** One file replaces the usual `Click`, `Press-Key`, `Focus-Game`
  helpers of LeviLauncher harnesses; scenarios stay as they are.

<p align="center"><img src="docs/public/showcase/how-it-feels.svg" alt="Your input goes to your apps; the script drives three background games over one pipe each" width="100%"></p>

## Install

Download `bedd-client-windows-x64.zip` from the
[releases](https://github.com/LiteLDev/BeDD/releases) and unzip it anywhere: it holds the module
(`BeDD.dll`) and the scripts. Harnesses copy the module into the game instance's `mods\` folder at the
start of a run and remove it afterwards ([see below](#plug-it-into-a-harness)); to try it by hand,
unzip it into the instance's `mods\` folder. Be.D.D. is not a lip package and is not listed on
LeviMC Mods.

Needs Minecraft Bedrock Edition **1.26.51** (GDK) with LeviLamina **26.51.x** (client), in a game
instance installed with LeviLauncher. Tests start that instance with `Start-BeDDGame.ps1`
rather than through the launcher ([see below](#several-games-at-once)). Only the test PC's game
needs it; servers and players do not.

### On a new PC

Nothing needs preparing by hand except the game itself. `Initialize-BeDD.ps1` (also run by
`Start-BeDDGame.ps1` and `clone-instance.ps1`) finds the instance and sets up whatever the PC
lacks, each step only once:

| Missing | What happens |
|---|---|
| LeviLauncher | the installed one is used; otherwise the latest release is downloaded to `%LOCALAPPDATA%\BeDD\tools` |
| the game | LeviLauncher opens and the script waits while you sign in with a Microsoft account that owns Minecraft and install 1.26.51 (a script cannot do this step; with `CI` set it fails with these instructions instead) |
| LeviLamina | installed into the instance with [lip](https://github.com/LiteLDev/lip), which is downloaded with the .NET runtime it needs (per user, no admin) |
| LeviLauncher's loader in the game exe | the instance is started once through LeviLauncher, which adds it, and the game is closed again. Without the loader the game would use your normal Minecraft data instead of the instance's |
| a clone (`-Slot 2` and up) | made from the base instance, and refreshed when the base instance has changed since |

```powershell
$i = & .\Initialize-BeDD.ps1 -Slot 2    # Name, Dir, ModsDir, PipeName of versions\<base>-hi2
```

BeDD finds LeviLauncher's data in `%APPDATA%\levilauncher.exe`, follows its "data folder" setting
(`base_root`), and picks the instance of 1.26.51 there (one with LeviLamina first). To point it
elsewhere, set `BEDD_INSTANCE` (instance name), `BEDD_VERSIONS_DIR`, `BEDD_LAUNCHER_DATA`,
`BEDD_LEVILAUNCHER` (LeviLauncher.exe) or `BEDD_LIP` (lip.exe).

A harness can fetch BeDD itself the same way instead of expecting a checkout somewhere:

```powershell
$bedd = if ($env:BEDD_DIR) { $env:BEDD_DIR } else { Join-Path $env:LOCALAPPDATA 'BeDD\client' }
if (-not (Test-Path "$bedd\harness-shim.ps1")) {
    $zip = Join-Path $env:TEMP 'bedd.zip'
    Invoke-WebRequest https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-client-windows-x64.zip -OutFile $zip
    Expand-Archive $zip (Split-Path $bedd) -Force; Rename-Item (Join-Path (Split-Path $bedd) 'BeDD') $bedd
}
```

## How to use

The PowerShell module ships next to the mod:

```powershell
Import-Module .\mods\BeDD\BeDD.psm1
Connect-BeDD                          # waits until the game is up
Send-BeDDKey E                             # open the inventory
Send-BeDDMouse click -RX 0.5 -RY 0.474     # click at a fraction of the window
Send-BeDDKey T; Send-BeDDText '/give @s apple 3'; Send-BeDDKey ENTER
Save-BeDDScreenshot C:\temp\shot.png       # works while the window is covered
```

| Command | What it does |
|---|---|
| `Connect-BeDD [-PipeName p]` | wait for the game and pick which game the next calls go to |
| `Send-BeDDKey E` | press a key; `-Action down`/`up` to hold, `-Vk 0x10` for any virtual key |
| `Send-BeDDText '...'` | type text (no key presses, so `t` does not open chat); `-DelayMs` per character |
| `Send-BeDDMouse click -RX 0.5 -RY 0.5` | `move`, `down`, `up`, `click`, `wheel` at `-X -Y` pixels or `-RX -RY` fractions; `-Button right` |
| `Send-BeDDRaw move -Dx 200` | turn the camera in the world; `click -Button right` to use an item |
| `Clear-BeDDInput` | release every key and button still held |
| `Save-BeDDScreenshot path.png` / `Get-BeDDBitmap` | the game's picture, even when covered |
| `Set-BeDDWindow 1600 900` | resize the game window without bringing it forward |
| `Get-BeDDState` | is the game up, how much input arrived, did it ever get the foreground |
| `Get-BeDDWorld` | in a world? which screen, mouse grabbed, menu open, the player's position and rotation (`gameHooks`) |
| `Set-BeDDLook 90 -30` | turn the player to a yaw and pitch in degrees (`gameHooks`) |
| `Set-BeDDInput -Z 1 -Sprint -TtlMs 2000` | walk, jump, sneak or sprint the player through the game's input systems for a while, whatever the key bindings (`gameHooks`) |
| `Set-BeDDSlot 3` / `Invoke-BeDDAction inventory` | select a hotbar slot; drop, drop all, open the inventory, switch perspective, pause (`gameHooks`) |
| `Set-BeDDButton attack -Action down -ReleaseOnBlockChange` | hold attack/use/pick for game ticks or until the targeted block breaks, without blocking the pipe (`gameHooks`) |
| `Send-BeDDPacket -Base64 ...` | the player sends a chat or command packet a script serialized (`allowSend`) |
| `Send-BeDDStack -Buffer ...` / `-View` | the player sends an item stack request a script serialized; `-View` shows the inventory as the client sees it (`allowStack`) |

Key names: letters, digits, `ESC`, `ENTER`, `BACK`, `TAB`, `SPACE`, `SHIFT`, `CTRL`, `ALT`, arrows
(`LEFT`...), `HOME`, `END`, `PGUP`, `PGDN`, `INSERT`, `DELETE`, `F1`..`F24`, `SLASH`, `GRAVE`,
`MINUS`, `EQUALS`.

### Several games at once

```powershell
pwsh -NoProfile -File clone-instance.ps1 -Slot 2                  # versions\1.26.51.01-hi2, ~90 MB, seconds
& .\Start-BeDDGame.ps1 -Instance 1.26.51.01-hi2                # starts it without bringing it forward
Connect-BeDD -PipeName BeDD-1.26.51.01-hi2   # the pipeName in that instance's config
```

`Start-BeDDGame.ps1 -Slot 2` (or `-Instance <base>-hi2`) makes the clone itself when it is missing. `clone-instance.ps1` takes slots 1 to 32; for a higher slot use `New-BeDDClone -Source <base> -Slot 41` from `BeDDSetup.psm1`.

A clone has its own `mods\`, logs, worlds and options. A few rules keep parallel runs from
tripping over each other:

- Give every instance its own lock file, and find your game by its **instance folder**, never just
  by the name `Minecraft.Windows` (or you close someone else's game).
- Start games as `Minecraft.Windows.exe` (`Start-BeDDGame.ps1` does): under another file name
  the game uses your normal Minecraft profile instead of the instance's.
- Make test worlds single-player with `Set-WorldSinglePlayer <worldDir>` (from the harness shim):
  games on one PC share an online sign-in, and multiplayer worlds opened at the same time fail with
  "An error has occurred" (code "Blaze").
- A run that joins a server online sets `$script:ExclusiveSignaling = $true` before `Start-Game`
  and gets the PC to itself while the other games close.

### The manager window

The first `Start-BeDDGame.ps1` of your logon session opens the **manager window**, and every
later run (from any terminal, harness or agent) reuses it. Every game shows up there as a live
picture, and nowhere else: its own window stays off screen, with no taskbar button and nothing in
Alt+Tab, so no windows pop up and vanish while several runs start and close games. It never takes
the focus.

- Each game is labelled with the project (and worktree) of the script that started it, its
  instance and pid, and whether input is arriving. `-Label` on `Start-BeDDGame.ps1` names it.
- **Closing the manager window closes every game in it** (it asks first). The games also go if the
  manager is killed, so nothing is left running in the background.
- Double-click a game to show only that one. Right-click it to open its `latest.log` or instance
  folder, or to close it.
- From a script: `BeDDManager.ps1 -List` lists the games, `-Game <pid> -Action close` closes one,
  `-Shutdown` closes the manager and every game without asking.

It needs PowerShell 7 (`pwsh`). A game never runs without it: a game that finds no manager window
starts one (`managerScript` in its config), and a game that no manager links within 90 s, or that
loses its manager for good, closes itself (the reason is in its `latest.log`).

### Plug it into a harness

`harness-shim.ps1` replaces the usual helpers of LeviLauncher harnesses (`Focus-Game`,
`Assert-GameForeground`, `Set-GameWindowSize`, `Move-Mouse`, `Click`, `Scroll`, `Press-Key`,
`Type-Text`, `Get-ClientBitmap`, same parameters) and wraps `Start-Game`/`Stop-Game`:

```powershell
. "$PSScriptRoot/lib.ps1"
. "$beddDir\harness-shim.ps1"
Register-BeDDNative ([MyE2E.Native])   # optional, see below
# after deploying your mod and moving the other mods aside:
Install-BeDDMod -ModsDir $script:ModsDir -PipeName 'BeDD-<instance>'
# ... and in finally, once the game is closed:
Uninstall-BeDDMod -ModsDir $script:ModsDir
```

Start games with `Start-BeDDGame.ps1` rather than the launcher, and keep `BeDD`
out of any "move other mods aside" step: it is installed fresh each run and removed afterwards (a
leftover copy would put a normally started game into the manager window too).

If your scenarios call the harness's own input class directly (`[MyE2E.Native]::Key(0x10, $false)`
and friends), give it these fields and a one-line guard at the top of each method;
`Register-BeDDNative` fills them:

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

`mods\BeDD\config\config.json`, usually written by the harness
(`Install-BeDDMod` does it). Without it the game still runs headless, with the defaults and no
`managerScript` (so only in a manager window that already runs). `Install-BeDDMod` and
`Start-BeDDGame.ps1` also switch the instance's LeviLamina to its built-in crash logger
(`modules.crashLogger.builtin` in `mods\LeviLamina\config\Config.json`): the default
`CrashLogger.exe` is a console program, and its console window would flash on the desktop at every
game start.

```json
{ "clientWidth": 1600, "clientHeight": 900, "pipeName": "BeDD", "managerScript": "C:\\BeDD\\BeDDManager.ps1" }
```

| Key | |
|---|---|
| `clientWidth`, `clientHeight` | game window size (0 = leave it), so clicks land in the same place every run |
| `pipeName` | one per game instance when several run at once |
| `loopbackOnly` | default `true`: the game stays on the PC, so no Windows Firewall prompt for each new instance, and game servers outside the PC are not queried (they could show up as "LAN worlds" ahead of your test world). Dedicated servers your harness starts still ask once per server folder; **Cancel** is fine |
| `managerScript` | path of `BeDDManager.ps1` (`Install-BeDDMod` writes it): the game starts the manager window with it when none runs |
| `mute` | default `true`: the game makes no sound |
| `trace` | log every input that reaches the game to `logs\latest.log`, for bug reports |
| `pipeInstances` | default `4`: scripts that may use the game's pipe at once (each one's held keys, buttons and `input` override are released when it disconnects); `1` = one at a time |
| `gameHooks` | default `false`: hook the game's own systems for the ops bedboo uses: `world` and `look` (`Get-BeDDWorld`, `Set-BeDDLook`), `input` (`Set-BeDDInput`: walk, jump, sneak and sprint whatever the key bindings), `slot`, `action` and `button` (`Set-BeDDSlot`, `Invoke-BeDDAction`, `Set-BeDDButton`: hotbar, drop/inventory/perspective/pause, attack/use/pick held for game ticks or until the targeted block breaks); see CONTRIBUTING.md |
| `tap` | default `false`: serve `\\.\pipe\<pipeName>-tap`, a binary stream of the game packets the client receives and sends, for [bedboo](https://github.com/LiteLDev/bedboo)'s transport `bedd` (see CONTRIBUTING.md, Tap protocol), with a join journal so a reader can attach in the middle of a session. It carries the whole session of the signed-in player, so only your user can open it. While it is on the client asks servers not to use its blob cache, so every chunk arrives in full |
| `tapLoopback` | default `true`, used only with `tap`: also tap singleplayer worlds and LAN worlds this game hosts (packets re-serialized; LAN guests' traffic is left out) |
| `tapJournalMiB` | default `128`: size cap of the tap's join journal; past it a late attach is incomplete (`complete:false`) |
| `allowSend` | default `false`: allow the `send` pipe op (`Send-BeDDPacket`), with which a script such as bedboo makes the client send chat and commands it serialized (any local program of yours could then do so as your player) |
| `sendAllow` | packet ids `send` may send, default `[9, 77]` (text, command_request); login, inventory_transaction, player_auth_input and item_stack_request are always refused |
| `allowStack` | default `false`: the `stack` op (`Send-BeDDStack`) lets a pipe client make the game send item stack requests (inventory moves, crafting, chest moves) for the signed-in player, as bedboo does in bedd mode. Turn it on only for such harnesses |

## More

The full documentation, with more videos and pictures, is at <https://docs.levimc.org/BeDD/>.

Building it yourself, the pipe protocol or how it works inside? See [CONTRIBUTING.md](CONTRIBUTING.md#development).

## License

[LGPL-3.0-or-later](COPYING.LESSER) © LeviMC. GameInput interface layouts follow Microsoft's public
`GameInput.h` (v2); no Microsoft code is included.
