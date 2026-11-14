# Be.D.D.

<img src="docs/public/logo.png" alt="" width="128" align="right">

English | [简体中文](README.zh-CN.md)

[![Release](https://img.shields.io/github/v/release/LiteLDev/BeDD)](https://github.com/LiteLDev/BeDD/releases)
[![License](https://img.shields.io/github/license/LiteLDev/BeDD)](COPYING.LESSER)
[![Docs](https://img.shields.io/badge/docs-docs.levimc.org%2FBeDD-blue)](https://docs.levimc.org/BeDD/)

Be.D.D. (Bedrock Deep Dive, after the H.D.D. of Zenless Zone Zero) runs end-to-end tests of
[LeviLamina](https://github.com/LiteLDev/LeviLamina) client mods **in the background**: your script or agent plays
Minecraft Bedrock over a local WebSocket while your mouse, keyboard and windows stay yours, several games at once.
It is a testing toolkit, not a mod for players: `BeDD.exe` (manager window and command line) clones, starts and
shows the games and puts a client-side LeviLamina module into each one for a run.

**Documentation: <https://docs.levimc.org/BeDD/>**

<p align="center"><img src="docs/public/showcase/desktop.webp" alt="Three headless Minecraft games react to a test script in the manager window" width="100%"></p>

> [!WARNING]
> A game with Be.D.D. in its `mods\` folder ignores your real mouse and keyboard and shows up only in the manager
> window. Do not leave it in a game you play.

## Install

Run `bedd-windows-x64-setup.exe` from the [releases](https://github.com/LiteLDev/BeDD/releases): per user, no
administrator rights, into `%LOCALAPPDATA%\Programs\BeDD`, `BeDD.exe` on `PATH`, **BeDD** in the Start menu.
`bedd-windows-x64.msi` is the same for silent installs (`msiexec /i bedd-windows-x64.msi /qn`);
`bedd-windows-x64.zip` needs no installer. Each contains:

| | |
|---|---|
| `BeDD/BeDD.exe` | the app: manager window and command line |
| `BeDD/mod/BeDD/` | the client module `BeDD.exe` puts into a game for a run |
| `BeDD/mod/BeDD-server/` | the module for a Bedrock Dedicated Server (client and server mode) |
| `BeDD/spec/` | `bedd.asyncapi.yaml`, the WebSocket protocol |

Needs Minecraft Bedrock **1.26.60** (GDK) with LeviLamina **26.60.x** (client), installed as an instance of
[LeviLauncher](https://github.com/LiteLDev/LeviLauncher) **1.1 or newer**. Not a lip package, not on LeviMC Mods;
only the test PC needs it.

`BeDD.exe` only reads your LeviLauncher instance (`LeviLauncher.exe instance list --json`; set `BEDD_LEVILAUNCHER`
when it is elsewhere) and never writes into it. Every game, slot 1 included, runs in a clone
`%LOCALAPPDATA%\BeDD\instances\<gameVersion>-<slot>`: hardlinked game files (copies across volumes), its own
`mods\`, logs, worlds and options, refreshed when the instance changes. All of Be.D.D.'s data lives under
`%LOCALAPPDATA%\BeDD` (`BEDD_HOME` moves it). `BeDD.exe instance list [--all] | clone --slot N | remove <name>`.

## Quick start

```
BeDD.exe launch --slot 2 --json
```

Starts the game of slot 2 headless in the manager window without taking the focus, waits until its WebSocket
listens, prints `pid`, `instance`, `url`, `token` and `label` (also in `%LOCALAPPDATA%\BeDD\run\<pid>.json`) and exits; the
game keeps running. `--detach` returns as soon as the process starts, with `url` and `token` null (read them from
that file or `BeDD.exe ps --json` once the game is up).

Drive the game in this order. Game-level answers are exact and cheap, a `ui` view is a few hundred characters, a
screenshot costs an agent far more tokens and pixel positions break.

1. **[bedboo](https://github.com/LiteLDev/bedboo)** (`npm i bedboo`), a mineflayer-style bot on the real client:

   ```js
   import { execFileSync } from 'node:child_process';
   import bedboo from 'bedboo';
   const { url, token } = JSON.parse(execFileSync('BeDD.exe', ['launch', '--slot', '2', '--json']));
   const bot = bedboo.createBot({ transport: 'bedd', bedd: { url, token } });
   bot.once('spawn', () => bot.chat('hello'));
   ```

   Without bedboo, send its ops yourself: `world`, `look`, `input`, `slot`, `action`, `button`, `settings`, `send`, `stack`,
   `timescale`, `join`, `tap`. A request `{id, op, ...}` gets `{id, ok:true, ...}` or `{id, ok:false, error, code}`
   once the game has handled it (Node 22+):

   ```js
   const ws = new WebSocket(`${url}?token=${token}`), waiting = new Map(); let id = 0;
   ws.onmessage = (e) => { const m = JSON.parse(e.data); waiting.get(m.id)?.(m); waiting.delete(m.id); };
   const call = (op, args = {}) => new Promise((r) => { waiting.set(++id, r); ws.send(JSON.stringify({ id, op, ...args })); });
   await new Promise((r) => { ws.onopen = r; });
   console.log(await call('ping'), await call('world')); // modes on offer; screen and player
   ```

2. **`ui`** for menus and screens bedboo does not cover: `await call('ui')` answers `{ok, view, screen, truncated}`,
   one line per control, `[ref]` on what takes an action:

   ```
   screen start_screen
   - button "Play" [e1]
   - button "Settings" [e2]
   - text "v26.60/LeviLamina-26.60.5"
   under: toast_screen, debug_screen, cubemap_background_screen
   ```

   `await call('ui', { action: 'click', ref: 'e1' })` clicks Play (also `setText`, `scroll`).
3. **Simulated input**, last resort: `key`, `text`, `mouse`, `raw`, `release`, `flush`, `focus`, `window`,
   `screenshot`.

Protocol: [`spec/bedd.asyncapi.yaml`](spec/bedd.asyncapi.yaml) (AsyncAPI 3.0) and the
[WebSocket reference](https://docs.levimc.org/BeDD/reference/websocket).

## Commands

`BeDD.exe help` lists every command. Every command takes `--json`, prints only its result on stdout and exits
non-zero with a hint on failure. `BeDD.exe ps` lists running games (`--json`: with `url`, `token`);
`BeDD.exe stop <pid>` closes one.

- **Several games**: `launch --slot 3 --label settings --json` per game; each slot has its own clone and WebSocket.
  Find your game by its `instance` (`ps --json`), never by the name `Minecraft.Windows`. Make test worlds
  single-player (`BeDD.exe world singleplayer <worldDir>`): games on one PC share an online sign-in, and multiplayer
  worlds opened at once fail with "An error has occurred" (code "Blaze").
- **Client and server**: `BeDD.exe server init|start|stop s1` runs a Bedrock Dedicated Server with BeDD's server
  module next to the game, whose speed it leads ([modes](https://docs.levimc.org/BeDD/guide/modes)).
- **Manager window**: `BeDD.exe` without arguments (the first `launch` starts it) is the only place games show up;
  it never takes the focus. Closing it closes every game in it (it asks first); a game it does not attach within
  30 s, or loses, closes itself. `BeDD.exe manager status|stop`.
- **Harness**:

  ```
  BeDD.exe instance clone --slot 2 --json           # where the clone is (its mods\ and worlds)
  BeDD.exe mod install 1.26.60.05-2                 # the BeDD module; settings: mod config
  BeDD.exe world singleplayer <the clone's world>   # after restoring your test world
  BeDD.exe launch --slot 2 --json                   # url + token
  BeDD.exe stop <pid>; BeDD.exe mod uninstall 1.26.60.05-2
  ```

  Deploy your own mod into the clone's `mods\` too, and keep `BeDD` out of any "move other mods aside" step.
  [BeetrootSkin](https://github.com/MiracleForest/BeetrootSkin/tree/main/tools/e2e) and
  [AlleviatedBackpacks](https://github.com/MiracleForest/AlleviatedBackpacks/tree/main/tools/e2e) have complete
  harnesses.

Settings (`mods\BeDD\config\config.json` of the clone): [Configuration](https://docs.levimc.org/BeDD/guide/configuration).
The token is the boundary: whoever has it can do what the signed-in player can; it is written only to
`%LOCALAPPDATA%\BeDD\run\<pid>.json`, readable by your user alone.

Building, the protocol internals and contributing: [CONTRIBUTING.md](CONTRIBUTING.md#development).

## License

[LGPL-3.0-or-later](COPYING.LESSER) © LeviMC. GameInput interface layouts follow Microsoft's public `GameInput.h`
(v2); no Microsoft code is included.
