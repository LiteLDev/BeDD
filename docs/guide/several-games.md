# Several games at once

Each game has its own clone and its own WebSocket, so one test PC runs several games side by side. A clone takes a
few seconds and about 90 MB (its game files are hardlinks to the base instance's).

A test that also needs a dedicated server on the PC starts one per run the same way, with its own name and ports:
see [Where the world runs](/guide/modes).

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

```powershell
BeDD.exe instance clone --slot 2            # 1.26.60.05-2, ~90 MB, seconds
BeDD.exe mod install 1.26.60.05-2           # the mod and its config into that clone
BeDD.exe launch --slot 2 --detach --json    # pid, url, token of that game
BeDD.exe launch --slot 3 --detach --json    # another one (clone and mod install for slot 3 first)
BeDD.exe ps                                 # both, with their instance and label
```

## Slots and clones

A **slot** is a number; its clone is `<gameVersion>-<slot>` under `%LOCALAPPDATA%\BeDD\instances`, e.g.
`1.26.60.05-2`. Slot 1 is a clone too: Be.D.D. never starts the base instance itself. Give each parallel run its own
slot (e.g. one per worktree or CI job); two runs on the same slot would share one clone and its worlds.

`launch --slot N` makes the clone when it is missing, and brings it up to date when the base has changed since.
`BeDD.exe instance clone|refresh|remove` do the same by hand ([Clones](/guide/new-pc#clones)). There is no upper
limit on slots.

## Rules for parallel runs

A clone has its own `mods\`, logs, worlds and options. A few rules keep parallel runs from tripping over each other:

- Find your game by its **instance** (`ps --json`, or the `run\<pid>.json` it wrote), never just by the process name
  `Minecraft.Windows` (or you close someone else's game). `BeDD.exe stop <pid>` closes only the game you name.
- Start games with `BeDD.exe launch`: it runs `Minecraft.Windows.exe` from the clone, which is what makes the game
  use the clone's data instead of your normal Minecraft profile.
- Make test worlds single-player with `BeDD.exe world singleplayer <worldDir>` right after restoring them: games on
  one PC share an online sign-in, and multiplayer worlds opened at the same time fail with "An error has occurred"
  (code "Blaze").
- A run that joins a server online uses `launch --exclusive-signaling`. Every running game holds the PC's signaling
  lock (in `%LOCALAPPDATA%\BeDD\locks`) for as long as it runs, shared by default; with `--exclusive-signaling` the
  new game waits until the others are closed and holds it alone, so no other game starts until it exits. The lock is
  held by the game process itself, so it is released even when the script that started it crashes.

## The manager window

Every game shows up in the **manager window**, and nowhere else: its own window stays off screen, with no taskbar
button and nothing in Alt+Tab, so no windows pop up and vanish while several runs start and close games. It never
takes the focus. Double-click `BeDD.exe` to open it; `BeDD.exe launch` opens it too when it is not running, and every
later run (from any terminal, harness or agent) reuses it.

![Look at the title bars: only the terminal is active](/showcase/desktop-poster.webp)

- Each game shows as a live picture, labelled with its `--label` (default: the project of the calling directory),
  its instance and pid, and whether input is arriving.
- **Closing the manager window closes every game in it** (it asks first). The games also go if the manager is
  killed, so nothing is left running in the background.
- Double-click a game to show only that one. Right-click it to close it, open its `latest.log` or instance folder, or
  copy its WebSocket URL.
- From a script: `BeDD.exe ps` lists the games, `BeDD.exe stop <pid>` closes one, `BeDD.exe manager status` tells
  whether the window runs, and `BeDD.exe manager stop` closes it and every game without asking.

A game never runs without it: a game that no manager attaches to within 30 s, or that loses its manager, closes
itself (the reason is in its `latest.log`). For a headless CI run without the window, set `manager` to `false`
([Configuration](/guide/configuration#keys)): the game then stays hidden and never closes by itself, so stop it with
`BeDD.exe stop`.
