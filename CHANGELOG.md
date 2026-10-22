# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/LiteLDev/LeviHeadlessInput/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/LiteLDev/LeviHeadlessInput/releases/tag/v0.1.0
