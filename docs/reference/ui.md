# UI tree

`ui` reads JSON UI and Ore UI screens as text, one line per control, and acts on a ref with the input ops' virtual
mouse, keys and text. Use after bedboo ops ([order](/guide/operation-modes)). Config `ui` (`true`) also gates
`oreui` and `mouse` with `ref`. Design: [#21](https://github.com/LiteLDev/BeDD/issues/21).

At 1600x900, characters: title 519 (old JSON 20,569), world list ~860 (raw DOM ~48,000), pause ~600, inventory ~950.

## The view

```
screen start_screen
- button "Play" [e1]
- button "Settings" [e2]
- text "v26.60/LeviLamina-26.60.5"
under: toast_screen, debug_screen, cubemap_background_screen
```

| Line | |
|---|---|
| `screen <name>` | per screen read; Ore UI: `screen <route> (Ore UI)`, e.g. `screen /play/all (Ore UI)`. Suffix ` (not ready: read again)` or ` (page not read: <why>)` |
| `- <role> "<text>"` / `- <role> <name>` | then ` = <value>` or ` checked`/` unchecked`, ` disabled`, ` focused`, ` selected`, then ` [<ref>]` if it takes an action. Indent = nesting |
| `- button hotbar_slot_button x9 [in e29]` | 6+ alike lines in a row; `{op:"ui", ref:"e29"}` lists them |
| `... <n> lines cut: ...` | past `maxNodes`; first 3/4, last 1/4 kept |
| `under: a, b` | screens below the front one (`scope:"active"` only) |
| `(nothing read on this page)` / `(no match)` | empty Ore UI page / no `find` hit |

Roles: `button`, `toggle`, `text_input`, `slider`, `scroll`, `text`, `item` (collection cell, e.g. inventory slot),
`group`.

Printed (`compact`): what can be seen, read or used. A button reads as its first visible label; a text repeating its
control is dropped; a group prints only if clickable and holding no other line; a JSON UI control named `button`,
`default`, `panel`, `image`, `label`, `content`, `control` or its role shows its group's name.

Refs (`e1`, ...): one per control (scene + path), shared by all reads and connections while its screen is on the
stack; never reassigned. A `snapshot` field is ignored.

## Reading

| Request | Reply |
|---|---|
| `{op:"ui"}` | `{view, screen (front), truncated}` |
| `{op:"ui", find:"quit"}` | flat lines whose text, name or value contains it (any case) |
| `{op:"ui", ref}` | `{view, truncated}`: the control (with ref) and all below, unfolded |
| `{op:"ui", ref, detail:true}` | `{node}`: all node fields + `ref`, `ancestors` (texts or names, nearest first) |
| `{op:"ui", format:"json"}` | `snapshot` (informational), `tick`, `screens`, `active`, `roots`, `nodes` by ref (+`parent`, `children`), `truncated`, `client{w,h}` |

| Field | Default | |
|---|---|---|
| `scope` | `active` | `active`, `all`, a screen name, or `scene-…` id |
| `maxNodes` | 80 lines; json: `uiMaxNodes` (1000) | 1..100000 |
| `depth` | all | levels below each screen root |
| `includeInvisible` | `false` | hidden controls too |
| `compact` | `true` | `false`: every control |
| `timeoutMs` | 3000 | 100..30000, then `not_ready` |
| `debug` | `false` | json/detail: components, vtables, visibility bits |

| Node field | |
|---|---|
| `role`, `name`, `text` | `name`: JSON UI control name; Ore UI `aria-label`/`title`/`alt`/`placeholder`/`name`/`data-testid` |
| `value` | text box text, slider 0..1, toggle state |
| `state` | `visible, enabled, hovered, focused, selected, pressed, editable, scrollable, checked` (null: no toggle) |
| `bounds`, `boundsNorm` | client px (`x`/`y`); fractions (`rx`/`ry`) |
| `actions` | `click`, `setText`, `scroll` |
| `surface` | screen id |
| `source` | `kind` jsonui\|oreui (whole page)\|dom; `path` (`name[k]` for same-name siblings), `type` (JSON UI type or tag), `collectionName`/`collectionIndex` (e.g. `inventory_items`/9), `url`, `route` |
| `hash` | of role, name, text, path, type, kind, url, route |

`screens[]`: `{id, kind jsonui|oreui|unknown, screen, interactive, modal}` + `rawScreen`, `route`, `ready:false`,
`pageError`, `debug` when set. `unknown` scenes (`in_game_play_screen`) are not read.

## Actions

```json
{"op": "ui", "action": "click", "ref": "e1"}
{"ok": true, "action": "click", "ref": "e1", "point": {"x": 800, "y": 428}, "changed": true,
 "target": "- button \"Play\" [e1]", "screen": "/play/all", "view": "screen /play/all (Ore UI)\n- button ..."}
```

| `action` | Fields (default) | Input |
|---|---|---|
| `click` | `button` (left), `holdMs`, `settleMs`, `afterMs` | `mouse click` |
| `setText` | `text`, `clear` (true), `submit` (false), `delayMs` | click; CTRL+A, BACK; `text`; ENTER |
| `scroll` | `notches` (1, positive = down) | `mouse wheel` |

All: `anchor` (center, top_left, top, top_right, left, right, bottom_left, bottom, bottom_right), `offsetX`/`offsetY`,
`force`, `timeoutMs`, `observeMs` (150, ≤5000: wait before reading the result).

Reply: `point`; `changed` (any text, state, value, place or front screen); `target` (its line now, null if gone);
`screen` (front); `view` only when another screen or Ore UI route came to the front.

### Mouse on a ref

`{"op":"mouse", "action":"move", "ref":"e68"}`: `mouse` aims at a ref (`anchor`, `offsetX`/`offsetY`, `force`;
`action` default `move`); the reply adds the action reply fields. Only `click`/`down` check for covering controls.

### Errors

The control must keep its role and name (JSON UI) or text (Ore UI).

| `code` | |
|---|---|
| `stale_ref` | unknown ref, screen closed, control gone or changed. `candidates`: ≤5 lines of same role and name/text (from the front screen if its own closed) |
| `occluded` | hidden; off window (`point`); or covered at the point (`occludedBy`, `point`) by a later input control on its screen (JSON UI) or an input-taking screen above. Controls ≥ half the window don't count. `force:true` overrides |
| `not_ready` | no answer within `timeoutMs`, no scene, or Ore UI page silent |
| `unsupported` | ref is a whole Ore UI page, or the UI could not be read |
| `bad_args` | `maxNodes`/`depth`/`timeoutMs` not integer, `format` not text/json, `find`/`ref` not string |
| `bad_request` | action without `ref`, unknown `action`, `setText` without `text` |

## 1.26.60

JSON UI: title (`start_screen`), HUD (`hud_screen`), chat, pause, inventory, exit dialog; no sliders or toggles seen.
Ore UI: world list (`/play/all`), settings (`/oreui-settings/...`). Hover hits the innermost control (inventory:
`item_button_ref` in `grid_item_for_inventory`). Read after entrance animations.

## Implementation

| Part | |
|---|---|
| JSON UI | SceneStack → `UIScene::mScreenView` → `VisualTree::mRootControl` → `UIControl::mChildren`/`mComponents`, on the client thread (`ClientInstance::update` hook). Types by vtable (no RTTI) (text: exported `render`; text box/slider/scroll: ScreenView's lists; button/toggle: 4/8 state weak_ptrs). SEH: bad layout → `unsupported` |
| Ore UI | topmost Ore UI page's DOM via the `oreui` script (≤4000 nodes, 120 chars), roles by tag, input `type` and ARIA role (`domRole`, `src/UiView.cpp`). A textless control takes its inner texts; a textless `act` 2 element beside a control with text is a layer of it |

## Ore UI screens: `oreui`

Raw Ore UI DOM, no refs (debugging).

| Field | Default | |
|---|---|---|
| `url` | any | wait for a view whose `location.href` (e.g. `ui://./hbui/index.html`) contains it; not the route |
| `maxNodes` | 2000 | 1..50000 |
| `maxText` | 200 | 8..4096 |
| `timeoutMs` | 3000 | 100..30000, then `not_ready` |
| `force` | `false` | allow a V8 major other than 9 |

Reply: `url`, `title`, `width`, `height`, `nodes` (document order), `truncated`, `findings` (`method`, `gameface`,
`v8`, `jsCalls`, `contextsSeen`, `missingExports`). Node: `tag`, `id`, `class`, `testId`,
`role`, `type`, `act`, `name`, `text` (own), `value`, `state` (`disabled, checked, selected, expanded, hidden, focused,
invisible`), `bounds` (client px), `children` (indexes). `act`: 1 = clickable by tag, ARIA role, tabindex, test id
or React/Preact handler; 2 = only `-interactive` hover styling (outside an `act` ancestor).

Hooks `v8::Function::Call`/`v8::Object::CallAsFunction` in `v8.dll` (V8 9.4; from first use) and runs a read-only
script on the page's next call (~300/s); one request at a time. `unsupported` (`findings`): other V8 major, missing
exports, script error, DOM > 15 MiB. A V8 crash (SEH) or endless script disables `oreui` and Ore UI in `ui` until restart.
