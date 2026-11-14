# Operation modes

All three modes are on at once over the same [WebSocket](/guide/usage). `ping` reports `defaultMode` and each mode's
ops in this build (`modes`).

| Mode | For | Ops |
|---|---|---|
| **bedboo** (default) | gameplay: moving, fighting, building, inventories, chat, commands | `world` `look` `input` `slot` `action` `button` `settings` `send` `stack` `timescale` `join` `tap` |
| **UI tree** | menus and screens bedboo does not cover | `ui`; `oreui` (raw Ore UI DOM, debug) |
| **Simulated input** | anything else | `key` `text` `mouse` `raw` `release` `flush` `focus` `window` `screenshot` |

## Which one to use

**bedboo first, then `ui`, then simulated input.**

- bedboo ops answer in game terms (numbers): exact, small, and independent of window size, GUI scale and key
  bindings.
- A `ui` view is a few hundred characters per screen; controls are clicked by `ref`, not pixels.
- A screenshot costs an agent ~500–1800 image tokens, needs another to verify, and its pixel positions break.

Keep a connection (or a small helper) open: each op is one JSON message. Check results with `world` or `ui`, not
screenshots.

## bedboo

[bedboo](https://github.com/LiteLDev/bedboo) is a mineflayer-compatible Bedrock bot library. With `transport: 'bedd'`
it turns `bot.dig`, `bot.lookAt`, `bot.setControlState`, `bot.chat`, `bot.clickWindow` and the rest into these ops and
reads packets from the `tap` ([setup](/guide/usage#with-bedboo); coverage: bedboo's BeDD guide).

Without bedboo, send the ops ([`call`](/guide/usage#a-small-client); fields: [bedboo ops](/reference/websocket#bedboo-ops)):

```js
await call('look', { yaw: 90, pitch: 30 });
await call('input', { move: { x: 0, z: 1 }, sprint: true, ttlMs: 2000 });
await call('button', { button: 'attack', action: 'down', releaseOn: 'blockChange' });
```

## UI tree

`ui` returns the screen in front (JSON UI or Ore UI) as text; act on a line's `ref`. Read forms, fields, roles and
errors: [UI tree](/reference/ui).

```
screen start_screen
- button "Play" [e1]
- button "Settings" [e2]
- text "v26.60/LeviLamina-26.60.5"
under: toast_screen, debug_screen, cubemap_background_screen
```

```js
await call('ui', { action: 'click', ref: 'e1' });   // reply: changed, target, screen; view when the screen changed
```

Refs last while their screen is on the stack, across connections. Actions use the virtual mouse and keyboard. On
`stale_ref`, read `ui` again.

## Simulated input

Fields: [input ops](/reference/websocket#input-ops); key names and positions: [Driving the game](/guide/usage#key-names).

```js
await call('key', { key: 'E' });                                  // open the inventory
await call('mouse', { action: 'click', rx: 0.5, ry: 0.474 });      // or { ref } from ui
await call('screenshot', { format: 'jpeg', region: { x: 0, y: 0, w: 480, h: 270 } }); // works while covered
```
