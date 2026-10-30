# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Setup for a new PC: `Initialize-BeDD.ps1` and `BeDDSetup.psm1` (`Initialize-BeDDInstance`, `Get-BeDDInstance`, …) find the LeviLauncher instance of Minecraft 1.26.51 and set up whatever is missing: LeviLauncher (downloaded from its releases when not installed), LeviLamina (installed with lip, which is downloaded with its .NET runtime), LeviLauncher's loader in the game exe, and clones. Only the game install itself is left to a person: LeviLauncher opens and the script waits for it. `Start-BeDDGame.ps1`, `clone-instance.ps1` and `tools/dev.ps1` run the same setup, and the harness shim exports it. Overrides: `BEDD_INSTANCE`, `BEDD_VERSIONS_DIR`, `BEDD_LAUNCHER_DATA`, `BEDD_LEVILAUNCHER`, `BEDD_LIP`.
- `Start-BeDDGame.ps1 -Slot N`; `-Instance` is optional (the instance BeDD finds).
- `tools/build.ps1`: builds with xmake, installing xmake, LLVM and the Visual Studio C++ build tools with winget when missing.
- `world` and `look` pipe ops (`Get-BeDDWorld`, `Set-BeDDLook`), off unless config `gameHooks` is on: `world` reports whether the client is in a world, its top screen, mouse grab, open menu, input mode and the local player's runtime id, eye position, rotation and hotbar slot; `look` turns the player to an absolute yaw/pitch (player_auth_input degrees) from the game's own thread and replies once it is within 0.5°.

### Fixed

- Nothing worked on a PC that differs from the author's: the scripts expected LeviLauncher at `Desktop\LeviLauncher.exe`, its instances in `%APPDATA%\levilauncher.exe\versions` (LeviLauncher's data folder setting was ignored), an instance named `1.26.51.01`, and ffmpeg in one user's winget folder. An instance never started from LeviLauncher also silently ran without its data isolation, since only LeviLauncher adds its loader to the game exe.
- An existing clone kept stale game files after its base instance was updated (`clone-instance.ps1` now refreshes them).
- `tools/dev.ps1` closed every Minecraft on the PC, including other runs' games; it only closes its own instance's now.

## [0.3.1] - 2026-10-30

### Changed

- The package is listed as Be.D.D. (`tooth.json` `info.name`), on LeviMC Mods too.

## [0.3.0] - 2026-10-30

### Changed

- **Renamed to Be.D.D. (Bedrock Deep Dive), after the H.D.D. of Zenless Zone Zero.** The repository is `LiteLDev/BeDD` and the lip package `github.com/LiteLDev/BeDD`; everything else follows, with no aliases for the old names:
  - mod folder and DLL `mods\BeDD\BeDD.dll`, build output `bin\BeDD\`, release asset `bedd-client-windows-x64.zip`;
  - control pipe `\\.\pipe\BeDD` (config `pipeName` default) and manager pipe `\\.\pipe\BeDD-Manager-<session id>`;
  - client `BeDD.psm1` with `Connect-BeDD`, `Disconnect-BeDD`, `Test-BeDD`, `Invoke-BeDD`, `Clear-BeDDInput` and `*-BeDD*` for every other `*-Headless*` cmdlet (`Send-BeDDKey`, `Get-BeDDState`, …); `Start-BeDDGame.ps1`, `BeDDManager.ps1`;
  - harness shim `Install-BeDDMod`, `Uninstall-BeDDMod`, `Register-BeDDNative`;
  - environment variables `BEDD_LABEL`, `BEDD_MANAGER`, `BEDD_PIPE`, `BEDD_RETURN_FOCUS`; harnesses use `BEDD_DIR` in place of `LEVI_HEADLESS_INPUT_DIR`.

### Added

- Config `mute` (default `true`): in background mode the game is silent. The mod hides focus loss from the game, so it never paused its audio, and headless games played menu music and sounds on the user's speakers. Taking a game over makes it audible again; `state` reports `audio`.

### Fixed

- A console window flashed on the desktop at every game start: LeviLamina starts its crash logger, `CrashLogger.exe`, as a console program, and the game has no console to give it. `Install-BeDDMod` and `Start-BeDDGame.ps1` (for background games) switch the instance's LeviLamina to its built-in crash logger (`modules.crashLogger.builtin`).

## [0.2.0] - 2026-10-29

### Added

- The manager window (`HeadlessManager.ps1`): one per logon session, started by the first `Start-HeadlessGame.ps1` (or the shim's `Start-Game`) and reused by every later run. Every headless game shows up there as a live picture instead of as a window of its own (off screen, no taskbar button, not in Alt+Tab), labelled with the project and worktree that started it. Closing the manager closes all its games (also when it is killed); right-click a game to take it over with the real mouse and keyboard, hand it back, open its log or close it. `-List`, `-Game <pid> -Action show|hide|close` and `-Shutdown` do the same from scripts. Config `manager` (default `true`), `Start-HeadlessGame.ps1 -Label/-NoManager`, `Install-HeadlessInputMod -NoManager`, `LEVI_HEADLESS_MANAGER=0`.
- `ping` and `state` report `managed` and `takenOver`.
- `state` reports `serverQueriesRefused`, and GameInput statistics report `virtualReadings`.

### Fixed

- A game could take the foreground for a moment when it started (with background mode on): the "focus gained" messages the mod sends activated the window when the game had been started from the user's foreground app (a terminal, an agent). They no longer activate it, and the user32 hooks are in place before the game creates its window.
- `enable()` could still hang for good while installing the hooks, most often with several games starting at once: a thread stopped by the hook pause while holding the process heap lock blocked the hooking code. The heap stays locked while the threads are paused now.
- Virtual clicks, wheel and camera were lost while nobody touched the real mouse (unattended runs stayed on the title screen with Play highlighted): GameInput has no mouse reading until the device reports input. The game now gets a virtual reading of the device instead.
- With `loopbackOnly`, a game could join a "LAN world" instead of the test world and hang at "Connecting to multiplayer game" (~97%): game servers queried through a proxy on the PC were listed as LAN worlds ahead of the test world. Game servers outside the PC are no longer queried.
- With `loopbackOnly`, outgoing TCP connections (WinHTTP) were bound to loopback and reached the internet only through a proxy on the PC. They are left alone now.
- `loopbackOnly` stays in force after the game is handed back (`Set-HeadlessBackground $false`, Ctrl+Shift+F12): sockets opened afterwards raised the firewall prompt.

## [0.1.3] - 2026-10-24

### Added

- A logo, and a recording of three games driven at once in the background, in the [LeviMC Mods gallery](https://levimc.org/mods/github.com/LiteLDev/BeDD/?tab=gallery).

### Changed

- The README is rewritten for people writing test scripts. How it works inside, the pipe protocol and build notes moved to [CONTRIBUTING.md](CONTRIBUTING.md).

## [0.1.2] - 2026-10-23

### Added

- Loopback-only sockets (`loopbackOnly`, on by default): in background mode `bind()` to 0.0.0.0/:: is turned into a loopback bind, so a new game instance (a new exe path) no longer raises the Windows Firewall "allow access" prompt that stopped unattended runs. Outgoing connections (local servers, the internet) are unaffected; nothing outside the PC can reach a test game. `state` reports `loopbackRebinds`.

- Safety net in the `Start-Game` wrapper: worlds of the instance written in the last 15 minutes (the test world a harness just restored) are made single-player before the game starts, so a harness that forgot `Set-WorldSinglePlayer` no longer breaks other projects' parallel runs with "Blaze". Older worlds are never touched.

## [0.1.1] - 2026-10-23

### Added

- Exclusive signaling lock also waits until no `Minecraft.Windows` process is left, so games started outside the lock cannot break online-multiplayer runs.
- Machine-wide signaling lock in `harness-shim.ps1`: every headless game holds it shared from `Start-Game` to `Stop-Game`; runs that join a server over NetherNet set `$script:ExclusiveSignaling = $true` and get the machine to themselves (each game's sign-in to the signaling service knocks out the previous one).

### Fixed

- Virtual mouse clicks, wheel notches and camera moves arrived once per mouse device (twice on a PC with two mice): the virtual state now goes to one device and the others report idle.
- `enable()` could hang while installing the hooks: all hooks of a batch are now installed during a single thread pause, and nothing is logged while threads are paused.

## [0.1.0] - 2026-10-22

### Added

- Control pipe (`\.\pipe\<pipeName>`, JSON lines) with `ping`, `state`, `key`, `text`, `mouse`, `raw`, `release`, `focus`, `window`, `background` and `flush`; input ops reply once the game thread has processed the input.
- Keyboard, text and pointer position dispatched through the game's own window procedure (subclassed window).
- Virtual GDK GameInput mouse (v2 `IGameInputReading` hooks): buttons, wheel and camera in menus and in the world; the real mouse and keyboard no longer reach a background game.
- Background mode: focus-loss messages swallowed (no pause), real client-area input swallowed, `ClipCursor`/`SetCursorPos`/`GetCursorPos`/`GetAsyncKeyState`/`GetKeyState`/`GetKeyboardState`/`GetForegroundWindow`/... report a virtual state, window activation hooks keep the game out of the foreground. Ctrl+Shift+F12 on the real keyboard turns it off.
- PowerShell client `HeadlessInput.psm1` (input, screenshots via `PrintWindow`, per-instance pipes).
- `harness-shim.ps1`: drop-in replacement of the usual LeviLauncher harness helpers, plus redirects for harness C# input classes and `Set-WorldSinglePlayer` (parallel instances share one signaling-service ID and knock each other out of multiplayer worlds: "An error has occurred", code "Blaze").
- `Start-HeadlessGame.ps1` (start an instance without activating it) and `clone-instance.ps1` (cheap clone instances for parallel games).
- Trace mode: message, API-hook and GameInput statistics, and every input event that reaches the game, marked synthetic or real.

[Unreleased]: https://github.com/LiteLDev/BeDD/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/LiteLDev/BeDD/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/LiteLDev/BeDD/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/LiteLDev/BeDD/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/LiteLDev/BeDD/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/LiteLDev/BeDD/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/LiteLDev/BeDD/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/LiteLDev/BeDD/releases/tag/v0.1.0
