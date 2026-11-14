# Driving the game

Each game serves a WebSocket on `127.0.0.1`: one JSON request per message, one reply once the game has handled it.
Drive it with bedboo first, then `ui`, then simulated input ([why](/guide/operation-modes#which-one-to-use)).

## Starting a game

```powershell
BeDD.exe launch --slot 1 --json
# {"pid":12345,"instance":"1.26.60.05-1","instanceDir":"…","url":"ws://127.0.0.1:53021/v1","token":"9f3c…","label":"…"}
```

`launch` clones, installs or updates the mod ([which build](/guide/installation#upgrading)), starts the game in the
background, opens the [manager window](/guide/several-games#the-manager-window), waits for the WebSocket, prints and
exits; the game keeps running. A running instance: exit 5.

| Flag | |
|---|---|
| `--slot N` / `<instance>` / `--dir <folder>` | clone `<base>-<N>` (default 1), a named instance, or any folder with `Minecraft.Windows.exe` |
| `--label <text>` | name in the manager window (default `BEDD_LABEL`, else the calling project) |
| `--exclusive-signaling` | wait for other games to close; keep the PC to itself ([rules](/guide/several-games#rules-for-parallel-runs)) |
| `--keep-mod` | keep the clone's mod |
| `--no-manager` | do not start the manager window |
| `--timeout` / `--lock-timeout` | wait for the WebSocket (`3m`) / signaling lock (`2h`) |
| `--detach` | return when the process starts; `url`/`token` are `null` |

`BeDD.exe ps` lists games; `BeDD.exe stop <pid>` saves and closes one.

## Which game

`BeDD.exe ps --json` (or `%LOCALAPPDATA%\BeDD\run\<pid>.json`, kept while the WebSocket listens) gives each game's
`url`, `token` and `instance`. Pick yours by `instance`, never by process name. Connect with `?token=<token>` or
`Authorization: Bearer <token>`; the token is new at each start. Details: [Connecting](/reference/websocket#connecting).

## With bedboo

```js
import bedboo from 'bedboo';
const bot = bedboo.createBot({ transport: 'bedd', bedd: { url: game.url, token: game.token } });
bot.once('spawn', () => bot.chat('/summon cow'));
```

## A small client

Without bedboo, use any WebSocket client: send `{id, op, ...}`, match the reply by `id`, skip binary (tap) frames. A
Node 24 `call(op, args)` helper, used in these guides: [Connecting](/reference/websocket#connecting). Ops by
mode: [Operation modes](/guide/operation-modes); every field: [WebSocket protocol](/reference/websocket).

## Key names

`key` takes names (`ESC`, `ENTER`, `F1`..`F24`, …: [input ops](/reference/websocket#input-ops)) or any `vk`. Chat:
bedboo `bot.chat('/summon cow')`; else `key T`, `text`, `key ENTER`.

## Positions

`mouse` takes `x`,`y` pixels, `rx`,`ry` fractions, or a `ui` `ref` (size-independent). With coordinates, fix the
window size (config `clientWidth`/`clientHeight` or `window`). `raw` turns the camera while the mouse is grabbed.

## Servers

```js
await call('join', { host: '127.0.0.1', port: 19140, wait: true });   // join, wait until in the world
(await call('join')).connection;                                      // none | local | remote
```

Works from the title screen or in a world (left first); fails on a join error or `timeoutMs` (60000). Servers off the
PC need the [Remote world mode](/guide/modes).

## Several connections

Up to `wsMaxClients` (8) connections per game, each served in order. When one closes, its `key`/`mouse` holds and
endless `button` holds are released; `button` `click`/`holdTicks`/`releaseOn` and `input` (until `ttlMs`/`ttlTicks`)
continue, so a one-shot caller can hang up. Details: [Several clients](/reference/websocket#several-clients).
