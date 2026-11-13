# Operation modes

A Be.D.D. game can be driven three ways. All three are on at the same time, over the same
[WebSocket](/guide/usage); they differ in what your test says and what it costs.

| Mode | You say | Good for | Ops |
|---|---|---|---|
| **bedboo** (default) | "walk to x, dig this block, say hi" | gameplay: moving, fighting, building, inventories, chat, commands | `world` `look` `input` `slot` `action` `button` `send` `stack` `timescale` `join` `tap` |
| **UI tree** | "click the button named Play" | menus and screens: settings, forms, your mod's own screens | `ui` |
| **Simulated input** | "press E, click at 0.5, 0.47, take a screenshot" | what the other two cannot reach | `key` `text` `mouse` `raw` `release` `flush` `focus` `window` `screenshot` |

`ping` tells which ops each mode has in this build:

```json
{ "ok": true, "protocol": 2, "defaultMode": "bedboo",
  "modes": { "bedboo": ["world", "look", "input", "slot", "action", "button", "send", "stack", "timescale", "join", "tap"],
             "ui": ["ui", "oreui"],
             "input": ["key", "text", "mouse", "raw", "release", "flush", "focus", "window", "screenshot"] } }
```

## Which one to use

**bedboo first, then the UI tree, then simulated input.** The difference is not only convenience:

- bedboo ops answer in game terms (the player is at 12.5, 64, -3; the block broke at tick 4012), so a test checks a
  number instead of a picture, and nothing breaks when the window size, the GUI scale or a key binding changes.
- The UI tree is text: a few hundred named nodes instead of a megapixel image. A test (or an AI agent writing one)
  finds "Play" by name and clicks it by reference.
- Simulated input works blind. To know where to click you need a screenshot, to know whether the click worked you
  need another one. For an AI agent each screenshot costs far more tokens than a `world` or `ui` reply, and a pixel
  position guessed from it can miss. Use it for what has no other way in, and check the result with `world` or `ui`
  rather than with another picture when you can.

## bedboo

[bedboo](https://github.com/LiteLDev/bedboo) is a mineflayer-compatible Bedrock bot library. With
`transport: 'bedd'` it does not speak the protocol itself: it drives the real client hosted by Be.D.D., turning
`bot.dig`, `bot.lookAt`, `bot.setControlState`, `bot.chat`, `bot.clickWindow` and the rest into these ops, and reading
the game's packets from the tap.

```js
import bedboo from 'bedboo';

const bot = bedboo.createBot({ transport: 'bedd', bedd: { url: game.url, token: game.token } });
bot.once('spawn', async () => {
  bot.chat('/give @s diamond_pickaxe');
  await bot.lookAt(bot.entity.position.offset(0, -1, 2));
  await bot.dig(bot.blockAt(bot.entity.position.offset(0, -1, 1)));
  bot.setControlState('forward', true);
});
```

See bedboo's BeDD guide for its options and which mineflayer APIs it covers. Without bedboo, send the ops yourself:

```js
await call('look', { yaw: 90, pitch: 30 });                           // degrees
await call('input', { move: { x: 0, z: 1 }, sprint: true, ttlMs: 2000 });
await call('button', { button: 'attack', action: 'down', releaseOn: 'blockChange' });
console.log((await call('world')).player.eye);                       // { x, y, z }
```

## UI tree

`ui` returns the game's screens as a tree, like an accessibility tree: each node has a `role` (`button`, `textbox`,
`label`, `toggle`, `slider`, `list`, `group`, …), a `name`, its `text` or `value`, its `state` (enabled, focused,
checked, …), its `bounds` in window pixels and fractions, and the `actions` it takes.

```js
const ui = await call('ui');
// { snapshot: 7, active: 'start_screen', roots: ['n1'], nodes: { n1: {…}, n12: { role: 'button', name: 'Play', … } } }
const [ref] = Object.entries(ui.nodes).find(([, n]) => n.role === 'button' && n.name === 'Play');
await call('ui', { snapshot: ui.snapshot, action: 'click', ref });

const form = await call('ui', { scope: 'active' });
const [field] = Object.entries(form.nodes).find(([, n]) => n.role === 'textbox');
await call('ui', { snapshot: form.snapshot, action: 'setText', ref: field, text: 'My world' });
```

- A `ref` belongs to its `snapshot`. After the screen changed, an action answers `stale_snapshot` or `stale_ref`:
  read `ui` again. `occluded` means something covers the node; `not_ready` that the screen is still building.
- `depth`, `scope`, `includeInvisible` and `maxNodes` (config `uiMaxNodes`, default 1000) keep a reply small;
  `truncated` says when nodes were left out.
- Actions go through the same virtual mouse and keyboard as simulated input, so the game sees a real click.
- The tree covers the game's JSON UI screens (most menus, containers, HUD, your mod's screens). Ore UI screens (the
  newer, web-based ones) show up as one node `{ role: 'group', source: { kind: 'oreui', url } }` for now: drive those
  with simulated input.

## Simulated input

The game gets keys, text, mouse moves, clicks and wheel turns as if a person were at the PC, without your own mouse
and keyboard being involved.

```js
import fs from 'node:fs';

await call('key', { key: 'E' });                                  // open the inventory
await call('mouse', { action: 'click', rx: 0.5, ry: 0.474 });      // a fraction of the window
await call('key', { key: 'T' }); await call('text', { text: '/give @s apple 3' }); await call('key', { key: 'ENTER' });
await call('raw', { action: 'move', dx: 200 });                    // turn the camera

const shot = await call('screenshot', { format: 'png', maxWidth: 960 });
fs.writeFileSync('shot.png', Buffer.from(shot.data, 'base64'));    // works while the window is covered
```

- `screenshot` takes `format` (`png` or `jpeg`, with `quality`), `maxWidth` (default 960) and a `region`; keep
  pictures small, and ask for a region when you only need part of the screen.
- `mouse` also takes a `ref` and `snapshot` from `ui` instead of coordinates: the click lands in the middle of that
  node.
- See [Driving the game](/guide/usage#key-names) for key names and positions.
