# Several games at once

Each game has its own clone (~90 MB, seconds) and WebSocket; a run that needs a [server](/guide/modes) starts its own.

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

```powershell
BeDD.exe launch --slot 2 --json
BeDD.exe launch --slot 3 --json
BeDD.exe ps
```

## Slots and clones

Slot N is clone `<gameVersion>-<N>` ([Clones](/guide/new-pc#clones)); slot 1 too, the base never runs. No upper
limit. Give each parallel run (worktree, CI job) its own slot: one slot means one shared clone and its worlds.

## Rules for parallel runs

- Find your game by **instance** (`ps --json`, `run\<pid>.json`), never by process name `Minecraft.Windows`; close it
  with `BeDD.exe stop <pid>`.
- Start games only with `BeDD.exe launch` (otherwise your normal profile's data is used).
- Run `BeDD.exe world singleplayer <worldDir>` after restoring a test world: games share one online sign-in, and
  simultaneous multiplayer worlds fail with "Blaze".
- Runs that join a server online use `launch --exclusive-signaling`: every game holds the signaling lock
  (`locks`) shared for its life; this one waits for the others to close and holds it alone. The game process holds
  it, so a crashed script does not leak it.

## The manager window

Games show only in the manager window: off screen, no taskbar button, not in Alt+Tab; the window never takes focus.
Double-click `BeDD.exe`, or `launch` opens it; later runs reuse it.

![Look at the title bars: only the terminal is active](/showcase/desktop-poster.webp)

- Each game: live picture, `--label`, instance, pid, input activity. Double-click: show only it. Right-click: close,
  open `latest.log` or the instance folder, copy the WebSocket URL.
- Closing it (asks first) or killing it closes every game.
- CLI: `BeDD.exe ps`, `stop <pid>`, `manager status`, `manager stop` (closes all without asking).

A game with no manager attached within 30 s, or that loses it, closes (reason in `latest.log`). Headless CI: config
`manager=false` ([Configuration](/guide/configuration#keys)); the game stays hidden, never closes itself; use
`BeDD.exe stop`.
