# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.3] - 2026-10-24

### Added

- A logo, and a recording of three games driven at once in the background, in the [LeviMC Mods gallery](https://levimc.org/mods/github.com/LiteLDev/LeviHeadlessInput/?tab=gallery).

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

[Unreleased]: https://github.com/LiteLDev/LeviHeadlessInput/compare/v0.1.3...HEAD
[0.1.3]: https://github.com/LiteLDev/LeviHeadlessInput/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/LiteLDev/LeviHeadlessInput/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/LiteLDev/LeviHeadlessInput/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/LiteLDev/LeviHeadlessInput/releases/tag/v0.1.0
