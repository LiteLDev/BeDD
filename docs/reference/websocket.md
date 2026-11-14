# WebSocket API

Contract: [bedd.asyncapi.yaml](https://docs.levimc.org/BeDD/bedd.asyncapi.yaml) (`spec/`, AsyncAPI 3.0; [Studio](https://studio.asyncapi.com/?url=https://docs.levimc.org/BeDD/bedd.asyncapi.yaml)).

## Connecting

Discovery file `%LOCALAPPDATA%\BeDD\run\<pid>.json` (root `BEDD_HOME`), present while listening; skip dead pids.
`BeDD.exe ps --json` lists the same.

```json
{"protocol": 2, "kind": "client", "pid": 23116, "url": "ws://127.0.0.1:53021/v1", "token": "9c1f…(64 hex)",
 "instance": "MyInstance-2", "instanceDir": "…\\instances\\MyInstance-2", "label": "smoke test", "modVersion": "0.4.0", "startedAt": "2026-11-13T09:41:07Z"}
```

| Item | |
|---|---|
| `kind` | `client` (game) or `server` ([server build](#server-target)) |
| Port | config `wsPort` (default 0: free port per start); read it from the file |
| Token | 32 random bytes, new per start. `Authorization: Bearer <token>` or `?token=<token>`; wrong/missing: HTTP 401 |
| Origin | handshake with `Origin` → HTTP 403 unless in config `allowOrigins` (default empty): browsers cannot connect |
| Limits | `127.0.0.1` only; `wsMaxClients` connections (8); UTF-8 JSON text messages ≤ 16 MiB; no compression |

Node.js 24 (its `WebSocket` sets no headers):

```js
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
const dir = join(process.env.BEDD_HOME ?? join(process.env.LOCALAPPDATA, 'BeDD'), 'run');
const game = readdirSync(dir).map((f) => JSON.parse(readFileSync(join(dir, f), 'utf8')))
  .find((e) => e.kind === 'client'); // and its pid alive
const ws = new WebSocket(`${game.url}?token=${game.token}`);
ws.onopen = () => ws.send(JSON.stringify({ id: 1, op: 'world' }));
ws.onmessage = (e) => { console.log(JSON.parse(e.data)); ws.close(); };
```

## Requests and replies

```json
{"id": 7, "op": "look", "yaw": 90, "pitch": 0}
{"id": 7, "ok": true, "yaw": 90, "pitch": 0, "tick": 5182, "turns": 1}
```

- `id`: any JSON value, echoed. Failure: `{id, ok:false, error, code?, key?}`; match `code` ([Errors](#errors)).
- Each connection is served in order; use more connections for parallel requests.
- Probe: an op without arguments (read-only for bedboo ops): `unknown op` = absent, `disabled` = off, `ok` = present.

## Choosing a mode

bedboo ops first, then `ui`, then input ops ([why](/guide/operation-modes)). `ping` lists them:

```json
{"ok": true, "protocol": 2, "pid": 23116, "kind": "client", "defaultMode": "bedboo",
 "modes": {"bedboo": ["world","look","input","slot","action","button","settings","send","stack","timescale","join","tap"],
           "ui": ["ui","oreui"], "input": ["key","text","mouse","raw","release","flush","focus","window","screenshot"]},
 "hwnd": 4589122, "managed": true, "client": {"w": 1280, "h": 720}, "cursor": {"x": 640, "y": 360}, "realForeground": false}
```

## bedboo ops

Config key `false` → `disabled`. [Implementation](/reference/how-it-works#bedboo-ops-inside-the-game).

| op | key | probe reply | action fields (default) |
|---|---|---|---|
| `world` | `gameHooks` | `inLevel`, `levelEpoch` (+1 per world), `topScreen`, `mouseGrabbed`, `showingMenu`, `inputMode` mouse\|touch\|gamepad\|undefined, `player{runtimeId, eye{x,y,z}, rot{yaw,pitch}, slot}` (null: no world), `tick` | none |
| `look` | `gameHooks` | `inLevel, yaw, pitch, tick` | `yaw`, `pitch`, `timeoutMs` (1000\*, ≤60000) |
| `input` | `gameHooks` | `active`, `via` hooks\|keys, `hooks{raw,state}`, `tick`, `applied`, `ticksLeft` | `move{x,z}`, `jump`, `sneak`, `sprint`, `ttlMs` (300) or `ttlTicks` |
| `slot` | `gameHooks` | `slot` (null: no world) | `slot` 0..8, `timeoutMs` (1000\*) |
| `action` | `gameHooks` | `names` | `name` drop\|dropAll\|inventory\|perspective\|pause\|swing; `hand` main\|off (swing); `timeoutMs` (1000\*) |
| `button` | `gameHooks` | `held` | `button` attack\|use\|pick; `action` down\|up\|click (click); `holdTicks` (click: 2); `releaseOn` blockChange; `timeoutMs` (1000\*) |
| `settings` | `gameHooks` | `renderDistance` (chunks; null before the first world), `inLevel` | `renderDistance` (chunks, 1..255) |
| `send` | `allowSend` | `allow` (`sendAllow` ids) | `buf`, `timeoutMs` (2000\*, ≤10000) |
| `stack` | `allowStack` | `enabled, via:"raw", stats{sent, answered, rejected, failed, resyncs}` | `buf`, `timeoutMs` (2000\*); or `view:true` |
| `tap` | `tap` | `enabled`, `attached`, `link{state up\|down, type remote\|local, address}`, `journal{frames, bytes, complete}`, `lastReplay`, `replay`, `counters{in, out, dropped, sent, readers}`, `hooks` | `action:"subscribe"` → [tap stream](/reference/tap) |
| `timescale` | none | [below](#timescale-game-speed) | `scale`, `paused`, `timeoutMs` (2000) |
| `join` | none | `mode`, `connection` none\|local\|remote, `host`, `port`, `inLevel`, `joining`, `last` | `host` (null: leave the world), `port` (19132), `wait`, `timeoutMs` (60000) |

\* At ≥20 ticks/s; slower, defaults scale by tick interval / 50 ms (up to the max). `not_in_level`: no client tick
within 20 tick intervals (≥1 s).

| op | Behaviour | Errors |
|---|---|---|
| `look` | absolute; replies `yaw, pitch, tick, turns` once both within 0.5°. Yaw 0 = +Z, 90 = -X; pitch -90 = up, clamped ±89.9 | `not_in_level`, `timeout` (with current yaw/pitch), `busy` (newer `look`), `bad_args` |
| `input` | `move`: `x` = left-right, `z` = forward-back, clamped [-1,1], diagonal normalized. Ends `ttlMs` (≤60000, longer cut) after the request (refresh to keep moving), on `ttlMs:0` or `release`. `ttlTicks` (≤1200) counts client ticks, also ends after 5 s without ticks. Outlives its connection. `via:"hooks"`: only with the mouse grabbed and no screen; `applied` = ticks written. `via:"keys"` (hook missing): W/A/S/D, SPACE, LSHIFT, LCTRL held like `key` (reach screens, break on rebinding) | `bad_args` (`ttlTicks` with `ttlMs`), `not_in_level` (`ttlTicks`, not ticking) |
| `slot`, `action`, `button` | answered after the next client tick applied them (`tick`). A paused singleplayer world does not tick: resume with `key` ESC | `not_in_level`, `busy` (screen open; `button up` still works), `timeout`, `bad_args`, `refused` |
| `slot` | hotbar key 1..9; replies once held (`slot`, `via` keys\|none) | |
| `action` | key released next tick: drop Q, dropAll Ctrl+Q, inventory E, pause Escape (default keymap); perspective: game call. `swing`: `LocalPlayer::swing`, arm swing with no attack, the client sends animate; works with a screen open; `hand:"off"` shows on this client only. Replies `name`, `hand` | |
| `button` | virtual GameInput left/right/middle; released after `holdTicks`, or (`releaseOn:"blockChange"`) when the block targeted on `down` changes, turns to air or is untargeted. `up` at once. Never blocks. Replies `held`, `block{x,y,z}`. On disconnect only an endless `down` is released; others finish, `releaseOn` at most 1200 more ticks | `refused`: no block targeted |
| `settings` | `renderDistance` sets Render Distance of every renderer (normal, Vibrant Visuals, ray tracing; not saved to options.txt) next client tick, then `ClientInstance::updateChunkRadius`: the client sends request_chunk_radius; the server's grant arrives as chunk_radius_update (tap). Replies `renderDistance` (rendered now), `requested`, `tick` | `not_in_level`, `timeout`, `bad_args`, `unavailable` |
| `send` | `buf`: base64 serverbound packet (varint header + body, bedrock-protocol), sent by the local player; replies `packetId, name`. Stats: `state.stats.send{sent, refused, failed, lastError}` | `bad_args` (unreadable, not sent), `refused` (not in `sendAllow`) |
| `join` | from title or a world (left first: `left`); answers "Online play is not rated" (Proceed) and "Trust this server?" (Trust and Join). `started:true` once connecting; `wait:true` → `joined:true`. Off-PC hosts need `allowHosts` (Remote mode). `host: null` leaves the world (Save & Quit / Disconnect): `left:true` (`wait`: once saved and back in the menus; else `started:true`), `left:false` in no world | `join_failed`, `timeout` (`screen`), `busy`, `not_allowed`, `stopped`, `no_client`, `unavailable`, `bad_args` |

### `stack`: item stack requests through the client

| Form | Reply |
|---|---|
| `{op:"stack", buf}` | `{via:"raw", requestIds, map:{"<caller id>": client id}}` once sent |
| `{op:"stack", view:true}` | `{inventory, ui, open?:{containerId, type, items}}`, entries `{slot, item, count, stackId}` |

- `buf`: one item_stack_request packet (id 147); other ids or unreadable bytes: `bad_args`.
- Each request gets a client id: odd, negative, down from `-0x40000001`. Stack ids naming caller ids are renamed (own
  and the last 256 earlier). `map` keys: caller ids as strings.
- The item_stack_response (tap) may precede the reply: hold it.
- No client prediction. Accepted request: the client sends `inventory_transaction {type: inventory_mismatch}`, the
  server resends the inventory. Rejected: nothing changes.
- The needed screen (inventory, chest) must be open: `action inventory`, `key E`, or use the block.

### `timescale`: game speed

Speed of a world this game hosts (singleplayer, LAN host): client and local server together. A request
(`scale`, `paused`) replies the probe once both sides run at it; probe also has `local`, `inLevel`.

| Field | |
|---|---|
| `scale` | 0.05..20, clamped (1 = 20 ticks/s) |
| `paused` | stops/resumes both sides, like the pause menu |
| `source` | `default`, `config` (`timeScale`, on entering a world), `local` (this API), `server` (a BeDD server) |
| `tps{client, server}` | ticks/s over the last 2 s; 0 paused; `server` null on a remote server. Measured local: 0.05→1, 0.5→10, 2→40, 3→60, 5→100, 10→200, 20→400 |
| `ticksPerSecond` | client timer base rate (20) |
| `effective{client, server}` | `{scale, paused, ticksPerSecond}` last applied per side (null before) |

Leaving a world resets to 1; entering applies `timeScale` (1). Errors: `not_in_level`, `remote` (not hosted here),
`server_led` (a BeDD server sets it: change it there), `timeout` (still applies later), `refused`, `bad_args`. A BeDD
server sends level_event 3007 (SimTimeScale, pos 0,0,0, `data` = float bits as int32, 0 = paused) on change and on
join; the client follows (0 pauses, keeping the scale).

### Who sends what (bedboo transport `bedd`)

The client sends every serverbound packet. bedboo's `lib/bedd/authority.js` drops client-owned packets, maps others
to ops (`input`, `look`, `slot`, `button`, `action`, `settings`, `stack`, `join`, key ESC), sends text and
command_request with `send` (fallback T/SLASH, `text`, ENTER), works Leave Bed and the sign editor through `ui`, sends
other `sendAllow` ids as is with `send`, and throws ERR_BEDD_UNSUPPORTED (BeddUnsupportedError) for the rest.

## ui ops

`ui` reads the screens as text with refs and acts on a ref; `oreui` returns the raw Ore UI DOM; `mouse` takes a `ref`.
Config key `ui` covers all three. Fields, replies and errors: [UI tree](/reference/ui).

## input ops

No config key. Reply after the game thread dispatched the input (`timeoutMs` 5000).

| op | fields (default) | |
|---|---|---|
| `key` | `key` or `vk` (1..254); `action` press\|down\|up (press); `holdMs` (60) | letters, digits, ESC, ENTER, BACK, TAB, SPACE, SHIFT, CTRL, ALT, LEFT/UP/RIGHT/DOWN, HOME, END, PGUP, PGDN, INSERT, DELETE, F1..F24, SLASH, GRAVE, MINUS, EQUALS |
| `text` | `text`, `delayMs` (0) | one `WM_CHAR` per UTF-16 unit |
| `mouse` | `action` move\|down\|up\|click\|wheel (move); `x`,`y` (client px) or `rx`,`ry` (fractions) or [`ref`](/reference/ui#mouse-on-a-ref); `button` left\|right\|middle (left); `notches` (1); `holdMs` (80, ≥50); `settleMs` (50, hover first); `afterMs` (50) | moves the virtual cursor, then GameInput buttons/wheel |
| `raw` | as `mouse`, with `dx`,`dy` (counts), `steps` (1, spread over frames) | camera while the mouse is grabbed |
| `release` | | releases all held keys/buttons (anyone's) and the `input` override |
| `focus` | | re-sends "focus gained" |
| `window` | `clientWidth`, `clientHeight` | resizes; replies `result` + window fields; never activates |
| `flush` | | waits until all posted input is dispatched |
| `screenshot` | `format` png\|jpeg, `quality`, `maxWidth` (960), `region{x,y,w,h}` | `{width, height, format, data}` (base64) |

## System ops

| op | reply |
|---|---|
| `ping` | [example](#choosing-a-mode); `hwnd` 0 before the window, `managed` = manager attached |
| `state` | window fields, `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput, loopbackRebinds, serverQueriesRefused, audio, tapLoopback, send, journal, sendsRefused}`, `clients`, `held[{vk, client}]`, `hooks{name: installed}`, `mode` ClientOnly\|ClientServer\|Remote, `connection{connection none\|local\|remote, inLevel, host, port}` |
| `manager` | `action:"attach"`: this connection is the manager; window kept off screen, taskbar, Alt+Tab. Config `manager` (true): game closes if none attaches within 30 s of its window or the attached one drops; `false`: hidden, never self-closes. Label: env `BEDD_LABEL` |
| `close` | `{ok}`, then the game closes |

## Errors

| code | meaning |
|---|---|
| `disabled` | config key `false` (`key`), or unloading |
| `not_in_level` | not in a world (server: not started) |
| `refused` | the game refused (id not in `sendAllow`, no block for `releaseOn`) |
| `timeout` | not done in `timeoutMs` (may still) |
| `bad_args` | invalid field values or unreadable bytes |
| `bad_request` | the op cannot act on it: binary message; `tap`/`manager` unknown action or no connection; `ui` action without `ref`, unknown `action`, `setText` without `text` |
| `internal` | a `ui` action's sub-op reply was unreadable (a bug) |
| `busy` | screen open, newer `look`, join running, tap taken |
| `stopped` | unloading |
| `join_failed` | join gave up (the client shows why) |
| `not_allowed` | off-PC host, `loopbackOnly`, not in `allowHosts` |
| `unavailable` | a needed hook did not install (game log) |
| `no_client` | no client instance yet |
| `remote` | world on a server not hosted here |
| `server_led` | a BeDD server leads the speed |
| `stale_ref`, `occluded`, `not_ready`, `unsupported` | [UI tree errors](/reference/ui#errors) |

No `code`: unknown op, not a JSON object, input not dispatched in time, invalid names (key, button).

## Several clients

Window-input ops are serialized across connections; `ping`, `state`, `manager`, `close`, `tap`, `world`, `look`,
`input`, `settings`, `send`, `stack`, `timescale`, `join`, `screenshot`, `oreui` and `ui` reads are not. On disconnect:

| Held | |
|---|---|
| `key`/`mouse` `down` | released (unless re-pressed by another connection) |
| `button` | see above |
| `input` override | runs to its deadline |
| manager | game closes |

Per-connection state in the mod: [code rules](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md#code-rules).

## Security

Anyone who can read the discovery file (the user's processes) can drive the game. Token compared in constant time.

- `send`: packets as the signed-in player, within `sendAllow` (default 9 text, 77 command_request, 78 command_block_update, 97 book_edit). Ids 1, 30, 144,
  147 are always refused. Widen only for your own server.
- `stack`: moves, crafts, drops and destroys the player's items on any server.
- tap: the whole session.

Turn off unneeded `allowSend`, `allowStack`, `tap`; never share a token.

## Server target

`--target_type=server` ([Building](/reference/building)) runs in BDS with LeviLamina: same WebSocket, `kind:"server"`,
other ops `unknown op`.

| op | fields (default) | reply |
|---|---|---|
| `ping` | | `protocol`, `pid`, `kind`, `target:"server"` |
| `state` | | `target`, `clients`, `loopbackOnly`, `stats{loopbackRebinds, serverQueriesRefused}` |
| `server` | | `tick`, `tps` (2 s), `paused`, `port` (`server-port`), `players[{name, xuid}]` |
| `timescale` | `scale` 0.05..20, `paused`, `timeoutMs` (5000); none = probe | `scale`, `paused`, `source` default\|config\|local, `tps{client:null, server}`, `ticksPerSecond` (20 × scale, 0 paused) |
| `command` | `command` (`/` optional), `timeoutMs` (10000) | `success`, `successCount`, `output[]`, as the console |

It broadcasts level_event 3007 (above) on change and to each spawned player. `tps.server` may trail
`ticksPerSecond`. Errors: `not_in_level`, `bad_args`, `timeout`.
