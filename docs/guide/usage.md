# Driving the game

A game with Be.D.D. serves a **WebSocket** on `127.0.0.1`. Your test opens it, sends one JSON request per message and
gets one JSON reply. Any language works; there is nothing to import from Be.D.D.

```powershell
BeDD.exe launch --slot 1 --detach --json
# {"pid":12345,"url":"ws://127.0.0.1:53021/v1","token":"9f3c…","instance":"1.26.60.05-1",…}
```

```js
// Node 24: WebSocket is built in
const ws = new WebSocket(`${url}?token=${token}`);
ws.onopen = () => ws.send(JSON.stringify({ id: 1, op: 'world' }));
ws.onmessage = (e) => console.log(JSON.parse(e.data)); // {"id":1,"ok":true,"inLevel":false,"topScreen":"start_screen",…}
```

Every request returns once the game has handled it (or answers `ok:false`). Nothing here moves the real cursor,
presses real keys or changes the foreground window.

![Hover and click where you aim: a virtual cursor brings up tooltips and clicks slots](/showcase/inventory-hover.webp)

## Three operation modes

Be.D.D. offers three ways to drive a game, all on at once. Pick the first one that can do the job:

1. **bedboo** (the default): game-level ops (`world`, `look`, `input`, `slot`, `action`, `button`, `send`, `stack`,
   `join`, `timescale`, `tap`), best used through [bedboo](https://github.com/LiteLDev/bedboo)'s mineflayer API.
2. **UI tree**: `ui` reads the menus as a tree of named buttons, text fields and labels; click them by reference.
3. **Simulated input**: `key`, `text`, `mouse`, `raw` and `screenshot`, like a person at the keyboard. The last
   resort: it needs coordinates and pictures.

[Operation modes](/guide/operation-modes) explains each one, with examples, and why the order matters.

## Starting a game

```powershell
BeDD.exe launch --slot 2 --label "my-mod e2e" --detach
```

`launch` starts the clone of that slot (making it when it is missing) as `Minecraft.Windows.exe`, without bringing it
forward, and opens the [manager window](/guide/several-games#the-manager-window) if it is not open yet. With
`--detach` it returns once the game's WebSocket listens and prints its `pid`, `url` and `token`; without it, it prints
the same and then waits until the game exits. The clone needs the mod (`BeDD.exe mod install <instance>`).

| Flag | |
|---|---|
| `--slot N` | the clone `<base>-<N>` (default 1) |
| `--label <text>` | the game's name in the manager window (default: the project of the calling directory) |
| `--world-mode <mode>` | `ClientOnly`, `ClientServer` or `Remote`: [where the world runs](/guide/modes) |
| `--exclusive-signaling` | wait until no other game is open, and keep the PC to itself while it runs ([Several games](/guide/several-games#rules-for-parallel-runs)) |
| `--detach` | return as soon as the game is up |

`BeDD.exe stop <pid>` closes a game (it saves and exits); `BeDD.exe ps` lists the running ones.

## Which game

Each running game writes `%LOCALAPPDATA%\BeDD\run\<pid>.json` while its WebSocket listens, and deletes it when it
exits:

```json
{ "protocol": 2, "kind": "client", "pid": 12345, "url": "ws://127.0.0.1:53021/v1", "token": "9f3c…",
  "instance": "1.26.60.05-2", "instanceDir": "C:\\Users\\me\\AppData\\Local\\BeDD\\instances\\1.26.60.05-2",
  "label": "my-mod e2e", "modVersion": "0.4.0", "startedAt": "2026-11-13T09:30:00Z" }
```

Read it from the pid `launch` printed, or ask `BeDD.exe ps --json`, which lists them all (and ignores files whose
game is gone). Find your game by its **instance**, never by the process name, so you never drive someone else's.

The `token` is required: send it as `?token=<token>` or as an `Authorization: Bearer <token>` header. A wrong or
missing token gets HTTP 401. Requests from web pages (an `Origin` header) get 403 unless the config lists them in
`allowOrigins`.

## A small client

```js
// e2e.mjs, Node 24
import { execFileSync } from 'node:child_process';

const games = JSON.parse(execFileSync('BeDD.exe', ['ps', '--json'], { encoding: 'utf8' }));
const game = games.find((g) => g.instance === '1.26.60.05-1');

const ws = new WebSocket(`${game.url}?token=${game.token}`);
await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });

let nextId = 1;
const pending = new Map();
ws.onmessage = (e) => {
  if (typeof e.data !== 'string') return;          // binary frames are tap frames
  const reply = JSON.parse(e.data);
  pending.get(reply.id)?.(reply);
  pending.delete(reply.id);
};
const call = (op, args = {}) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, (r) => (r.ok ? resolve(r) : reject(Object.assign(new Error(r.error), r))));
  ws.send(JSON.stringify({ id, op, ...args }));
});

console.log(await call('ping'));                   // protocol 2, defaultMode "bedboo", modes {…}
await call('timescale', { scale: 3 });
```

The same in Python, with the [`websockets`](https://pypi.org/project/websockets/) package:

```python
import asyncio, json, subprocess
from websockets.asyncio.client import connect

game = json.loads(subprocess.check_output(["BeDD.exe", "ps", "--json"]))[0]

async def main():
    async with connect(f"{game['url']}?token={game['token']}") as ws:
        async def call(op, **args):
            await ws.send(json.dumps({"id": 1, "op": op, **args}))
            reply = json.loads(await ws.recv())
            if not reply["ok"]:
                raise RuntimeError(reply["error"])
            return reply
        print(await call("world"))

asyncio.run(main())
```

The full list of ops, fields and replies is in the [WebSocket protocol](/reference/websocket) reference, and
machine-readable in `spec\bedd.asyncapi.yaml`.

## Ops

| Op | Mode | What it does |
|---|---|---|
| `ping` / `state` | | is the game up, its window, the modes it offers; `state` adds how much input arrived and whether it ever got the foreground |
| `world` | bedboo | in a world? which screen, mouse grabbed, menu open, the player's position, rotation and slot |
| `look` | bedboo | turn the player to a yaw and pitch in degrees |
| `input` | bedboo | walk, jump, sneak or sprint through the game's input systems for a while, whatever the key bindings |
| `slot` / `action` | bedboo | select a hotbar slot; drop, drop all, open the inventory, switch perspective, pause |
| `button` | bedboo | hold attack, use or pick for game ticks or until the targeted block breaks |
| `send` | bedboo | the player sends a chat or command packet a script serialized |
| `stack` | bedboo | the player sends an item stack request a script serialized; `view:true` shows the inventory as the client sees it |
| `join` | bedboo | join a server by address, from the title screen or from inside a world |
| `timescale` | bedboo | the game speed of a world this game hosts, with the ticks per second measured; `paused` stops the ticks |
| `tap` | bedboo | the packets the client receives and sends, as binary messages on this WebSocket ([Tap protocol](/reference/tap)) |
| `ui` | UI tree | the menus as a tree of nodes; `click`, `setText`, `scroll` one by its `ref` |
| `key` / `text` | input | press a key (`action` down/up to hold, `vk` for any virtual key); type text without key presses |
| `mouse` / `raw` | input | move, click, wheel the virtual mouse at a point (`x`,`y` pixels or `rx`,`ry` fractions); `raw` turns the camera |
| `screenshot` | input | the game's picture as PNG or JPEG, even while covered |
| `release` / `flush` / `focus` / `window` | input | release every held key and button; wait until all input is handled; resend focus; resize the window |
| `close` | | close the game |

The bedboo and UI tree ops can be turned off with [feature keys](/guide/configuration#feature-keys); an op whose key
is off answers `code:"disabled"`.

## Key names

Letters, digits, `ESC`, `ENTER`, `BACK`, `TAB`, `SPACE`, `SHIFT`, `CTRL`, `ALT`, arrows (`LEFT`, `UP`, `RIGHT`,
`DOWN`), `HOME`, `END`, `PGUP`, `PGDN`, `INSERT`, `DELETE`, `F1`..`F24`, `SLASH`, `GRAVE`, `MINUS`, `EQUALS`. For any
other key pass its virtual key code as `vk`.

## Positions

`mouse` takes a point in the game window's client area: `x`,`y` in pixels, or `rx`,`ry` as fractions of its width
and height. Fix the window size (config `clientWidth`/`clientHeight`, or the `window` op) so a click lands in the
same place every run. Better still, click a `ui` node by its `ref`: that does not depend on the window size at all.

The mouse buttons and the wheel go to a virtual mouse the game reads instead of yours. `raw` moves that mouse by raw
counts, which turns the camera while the game has grabbed the mouse (in a world, no screen open).

## Chat and commands

![The text op types '/summon sheep' and '/summon cow' into chat](/showcase/chat-summon.webp)

`text` types characters without key presses, so open the chat first (`key T` or `SLASH`) and send it with
`key ENTER`. The game runs commands typed this way like a player typed them. With bedboo, `bot.chat('/summon cow')`
does the same through `send`, without the chat screen.

## Servers

`join` makes the client join a server the way "Add server" and "Play" in the Servers tab do, without clicking: from
the title screen, or from inside a world, which it leaves first. It returns once the client connects; with
`wait:true` once it is in the server's world, and it fails when the join fails (the game shows why) or `timeoutMs`
(default 60000) runs out. Without `host` it reports where the client plays: `connection` is `none`, `local` (a world
of its own) or `remote` (`host`, `port`), plus the [world mode](/guide/modes).

```js
await call('join', { host: '127.0.0.1', port: 19140, wait: true });   // a dedicated server on this PC
(await call('join')).connection;                                      // 'remote'
```

A server off the PC needs the Remote world mode (`BeDD.exe launch --world-mode Remote`, plus `allowHosts`): the game
reaches only the servers on the PC otherwise.

## Several connections

Several scripts may use the same game at once (config `wsMaxClients`, default 8): a bedboo bot, a UI checker and the
manager window, say. Each connection is handled in order. The keys and buttons a connection still holds, and the
`input` it set, are released when it closes.
