# UI tree

The `ui` op returns the game's UI as a tree of controls, like an accessibility tree, and acts on a
control by reference. It is the third way to drive a game, next to bedboo's game ops and plain
keyboard and mouse input: a script asks what is on screen and clicks "the Resume Game button"
instead of a pixel, and checks a control's state instead of comparing screenshots. Actions are
not shortcuts: they move the virtual pointer to the control and use the same mouse, key and text
input as the `mouse`, `key` and `text` ops. Design: [issue #21](https://github.com/LiteLDev/BeDD/issues/21).

Config `ui` (default `true`) turns it on; off, `{op:"ui"}` and `mouse` with a `ref` answer
`{ok:false, code:"disabled", key:"ui"}`. `uiMaxNodes` (default `1000`) caps the nodes of a snapshot.

## Snapshot

```json
{ "op": "ui" }
```

| Field | Default | |
|---|---|---|
| `scope` | `active` | `active` (the screen in front), `all`, a screen name or a screen id (`scene-…`) |
| `depth` | all | levels below each screen's root |
| `includeInvisible` | `false` | also hidden controls (`state.visible:false`) |
| `compact` | `true` | only controls that can be seen, read or used, and the groups that hold several of them; `false` = every control |
| `maxNodes` | `uiMaxNodes` | node cap: past it `truncated:true` |
| `timeoutMs` | `3000` | wait for the client thread; past it `code:"not_ready"` |
| `debug` | `false` | per node: the components and their vtables, raw visibility bits |

Reply:

```json
{
  "ok": true, "snapshot": "ui-12", "active": "scene-2a1f0c3d590",
  "screens": [
    { "id": "scene-2a1f0b7a810", "kind": "unknown", "screen": "in_game_play_screen", "interactive": false, "modal": true },
    { "id": "scene-2a1f0c3d590", "kind": "jsonui", "screen": "pause_screen", "interactive": true, "modal": false }
  ],
  "roots": ["e1"],
  "nodes": {
    "e5": {
      "ref": "e5", "role": "button", "name": "return_to_game_button", "text": "Resume Game", "value": null,
      "state": { "visible": true, "enabled": true, "hovered": false, "focused": false, "selected": false,
                 "pressed": false, "editable": false, "scrollable": false, "checked": null },
      "bounds": { "x": 578, "y": 300, "w": 443, "h": 90 },
      "boundsNorm": { "x": 0.3614, "y": 0.3333, "w": 0.2772, "h": 0.1 },
      "actions": ["click"], "children": ["e6"], "parent": "e4", "surface": "scene-2a1f0c3d590",
      "source": { "kind": "jsonui", "type": "button", "path": "/pause_screen/.../return_to_game_button" },
      "hash": "1c0a6e2f9b3d4a71"
    }
  },
  "truncated": false, "client": { "w": 1600, "h": 900 }
}
```

- `screens` is the scene stack, bottom to top. `kind`: `jsonui` (read control by control), `oreui`
  (one opaque node), `unknown` (other scenes, such as the in-game play screen: not read).
- `role`: `button`, `toggle`, `text_input`, `slider`, `scroll`, `text`, `item` (a collection cell
  that takes input without a known button component) or `group`.
- `name` is the control's name in its JSON UI definition; `text` its drawn text, localized (a
  button's or a toggle's: its first visible label); `value` a text box's text, a slider's 0..1, a
  toggle's checked state.
- `bounds` are client pixels, the space of `mouse` `x`/`y`; `boundsNorm` window fractions, the space
  of `rx`/`ry`.
- `source.path` is the control path (`name[k]` when siblings share a name); collection cells add
  `collectionName` and `collectionIndex` (an inventory slot: `inventory_items` / 9).
- `hash` covers role, name, text and source: a ref whose control changed any of them is stale.
- Each connection keeps its last snapshot only. Refs (`e1`, `e2`, … in paint order) are valid for it.

## Actions

```json
{ "op": "ui", "snapshot": "ui-12", "action": "click", "ref": "e5" }
```

| `action` | Fields | |
|---|---|---|
| `click` | `button` (`left`), `holdMs`, `anchor`, `offsetX`/`offsetY`, `force` | `mouse click` at the control |
| `setText` | `text`, `clear` (`true`), `submit` (`false`), `delayMs` | click to focus; with `clear` CTRL+A, BACK; the `text` op; with `submit` ENTER |
| `scroll` | `notches` (`1`; positive scrolls down) | `mouse wheel` over the control |

The `mouse` op takes `ref` + `snapshot` in place of `x`/`y`, for hover and drag:

```json
{ "op": "mouse", "action": "move", "ref": "e68", "snapshot": "ui-12" }
```

`anchor` (`center`, `top_left`, `top`, `top_right`, `left`, `right`, `bottom_left`, `bottom`,
`bottom_right`) and `offsetX`/`offsetY` (pixels) pick the point; the rest is the plain `mouse` op.

Before acting, BeDD reads the UI again and finds the control by its path:

| `code` | |
|---|---|
| `stale_snapshot` | not this connection's last snapshot (`snapshot`: the last one) |
| `stale_ref` | the control is gone or its hash changed (`candidates`: controls with the same role and name or text, on the screen now in front). Take a new snapshot |
| `occluded` | hidden, outside the window, or (for `click`, `setText`, `scroll`, `mouse` `down`/`click`) a control painted above it, or on a screen above that takes input, sits at the point (`occludedBy`). `force:true` acts anyway |
| `not_ready` | the client thread did not answer in time, or no scene yet |
| `unsupported` | an Ore UI node, or a UI this game version lays out differently than expected |

The reply has `point`, `target` (the node as it is after the action), `screen` (the screen in front)
and `changed` (whether texts, states, places or the screen in front changed). `observeMs` (`150`)
is how long it waits for the game to draw the result before reading it.

## What BeDD reads (1.26.60)

| Screen | Stack |
|---|---|
| title (`start_screen`), HUD (`hud_screen`), chat, pause, inventory, the exit dialog | JSON UI |
| world list (`/play/all`), settings (`/oreui-settings/...`) | Ore UI: one node with `source.url` and `source.route` |

Hover lands on the innermost control under the pointer: in the inventory, a slot's
`grid_item_for_inventory` holds an `item_button_ref` that shows `hovered`. A screen's entrance
animation moves its controls for a moment: take the snapshot once the screen has settled.

The game has no RTTI, so BeDD tells component types apart by their vtables: a text component by
its exported `render`, text boxes, sliders and scroll views from ScreenView's lists of them, buttons
and toggles by the state controls they point to. Sliders and toggles were not seen in a JSON UI
screen of 1.26.60 (the settings are Ore UI).

## Ore UI screens: `oreui`

`ui` shows an Ore UI screen as one node. To see inside it, send `{"op":"oreui"}`: BeDD runs a
read-only script in the Ore UI view's page and returns its DOM.

```json
{"id": 23, "op": "oreui"}
```

The reply has `url` (the route, such as `/play/all`), `title`, `width` and `height`, `nodes` in
document order and `truncated`. A node has `tag`, and when present `id`, `class`, `testId`
(`data-testid`), `role`, `name` (`aria-label`, `title`, `alt`, `placeholder` or `name`), `text`
(its own text), `value`, `state` (`disabled`, `checked`, `selected`, `expanded`, `hidden`,
`focused`, `invisible`), `bounds` in game client pixels and `children` (indexes into `nodes`).

Ore UI nodes have no refs. To press one, send `mouse` with `action: "click"` at the centre of
its `bounds`. This is how to open a world from the world list:

```js
const dom = await call('oreui')
const tile = dom.nodes.find(n => n.text === 'My World' && n.bounds?.w > 0)
await call('mouse', { action: 'click', x: tile.bounds.x + tile.bounds.w / 2, y: tile.bounds.y + tile.bounds.h / 2 })
```

The script runs the next time the view runs any script of its own, which an Ore UI screen does
many times a second. `timeoutMs` (`3000`) is how long to wait, and `url` waits for a view whose URL contains it; with no Ore UI screen on top the
op ends in `not_ready`. `maxNodes` (`2000`) and `maxText` (`200`) limit the reply.

`oreui` calls the V8 engine in the game's `v8.dll` (V8 9 in 1.26.60). On another major version it
answers `unsupported` with the version in `findings.v8`; `"force": true` tries anyway. If a call
into V8 ever crashes, BeDD catches it and turns `oreui` off until the game restarts.
