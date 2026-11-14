# WebSocket API

Every game that runs BeDD serves a WebSocket on `127.0.0.1`. Test scripts, agents,
[bedboo](https://github.com/LiteLDev/bedboo) and the BeDD manager all drive the game through it, and anything that
can open a WebSocket can do the same. This page is the human reference; the machine-readable contract is the
AsyncAPI document [bedd.asyncapi.yaml](https://docs.levimc.org/BeDD/bedd.asyncapi.yaml) (the same file as
`spec/bedd.asyncapi.yaml` in the repository), with a JSON Schema for every request and reply. Open it in
[AsyncAPI Studio](https://studio.asyncapi.com/?url=https://docs.levimc.org/BeDD/bedd.asyncapi.yaml) to browse it, or
generate a client from it.

## Connecting

### Finding a game

While its WebSocket server listens, a game writes a discovery file `%LOCALAPPDATA%\BeDD\run\<pid>.json` (the root is
`BEDD_HOME` when that is set) and deletes it when it exits. A file whose process is gone is stale: skip it.

```json
{
  "protocol": 2,
  "kind": "client",
  "pid": 23116,
  "url": "ws://127.0.0.1:53021/v1",
  "token": "9c1f…64 hex characters…",
  "instance": "MyInstance-2",
  "instanceDir": "C:\\Users\\me\\AppData\\Local\\BeDD\\instances\\MyInstance-2",
  "label": "smoke test",
  "modVersion": "0.4.0",
  "startedAt": "2026-11-13T09:41:07Z"
}
```

`kind` is `client` for a game and `server` for a Bedrock Dedicated Server running the server build.
`BeDD.exe ps --json` lists the same endpoints. The port is config `wsPort` (default 0: the system picks a free port at
each start), so always read it from the file. The token is 32 random bytes, new at every start.

### Authentication and the Origin rule

- Send the token as `Authorization: Bearer <token>` on the handshake, or as `?token=<token>` in the URL for clients
  that cannot set headers. A missing or wrong token is HTTP 401.
- A handshake that carries an `Origin` header is refused with HTTP 403, unless that origin is listed in config
  `allowOrigins` (default empty). Browsers always send `Origin`, so a web page cannot reach a game; scripts and
  programs do not send it.
- The server binds `127.0.0.1` only and accepts at most `wsMaxClients` connections (default 8).
- Text messages are UTF-8 JSON, at most 16 MiB each. No extensions (no compression).

### Node.js 24

Node 24 has a global `WebSocket`; it cannot set headers, so pass the token in the URL.

```js
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const runDir = join(process.env.BEDD_HOME ?? join(process.env.LOCALAPPDATA, 'BeDD'), 'run');
const alive = (pid) => { try { process.kill(pid, 0); return true; } catch { return false; } };
const game = readdirSync(runDir)
  .map((f) => JSON.parse(readFileSync(join(runDir, f), 'utf8')))
  .find((e) => e.kind === 'client' && alive(e.pid));

const ws = new WebSocket(`${game.url}?token=${game.token}`);
let next = 1;
const pending = new Map();
ws.onmessage = (e) => {
  const reply = JSON.parse(e.data);
  pending.get(reply.id)?.(reply);
  pending.delete(reply.id);
};
const call = (op, args = {}) =>
  new Promise((resolve) => {
    const id = next++;
    pending.set(id, resolve);
    ws.send(JSON.stringify({ id, op, ...args }));
  });

await new Promise((r) => (ws.onopen = r));
console.log(await call('ping'));
console.log(await call('world'));
ws.close();
```

### Python

With the [websockets](https://pypi.org/project/websockets/) package:

```python
import asyncio, glob, json, os, websockets

def find_game():
    root = os.environ.get("BEDD_HOME") or os.path.join(os.environ["LOCALAPPDATA"], "BeDD")
    for path in glob.glob(os.path.join(root, "run", "*.json")):
        with open(path, encoding="utf-8") as f:
            ep = json.load(f)
        if ep["kind"] == "client":
            return ep  # check that ep["pid"] is alive in real code

async def main():
    game = find_game()
    headers = {"Authorization": f"Bearer {game['token']}"}
    async with websockets.connect(game["url"], additional_headers=headers) as ws:
        await ws.send(json.dumps({"id": 1, "op": "ping"}))
        print(json.loads(await ws.recv()))

asyncio.run(main())
```

### C#

```csharp
using System.Net.WebSockets;
using System.Text;
using System.Text.Json.Nodes;

var runDir = Path.Combine(Environment.GetEnvironmentVariable("BEDD_HOME")
    ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BeDD"), "run");
var game = Directory.EnumerateFiles(runDir, "*.json")
    .Select(f => JsonNode.Parse(File.ReadAllText(f))!)
    .First(e => (string?)e["kind"] == "client"); // check that the pid is alive in real code

using var ws = new ClientWebSocket();
ws.Options.SetRequestHeader("Authorization", $"Bearer {game["token"]}");
await ws.ConnectAsync(new Uri((string)game["url"]!), CancellationToken.None);

await ws.SendAsync(Encoding.UTF8.GetBytes("""{"id":1,"op":"ping"}"""), WebSocketMessageType.Text, true, default);
var buffer = new byte[1 << 20];
var result = await ws.ReceiveAsync(buffer, default); // a large reply may span several frames
Console.WriteLine(Encoding.UTF8.GetString(buffer, 0, result.Count));
```

## Requests and replies

Each text message is one JSON object. A request names its `op` and may carry an `id` (any JSON value), which the
reply echoes:

```json
{"id": 7, "op": "look", "yaw": 90, "pitch": 0}
{"id": 7, "ok": true, "yaw": 90, "pitch": 0, "tick": 5182, "turns": 1}
```

A failure is `{"id", "ok": false, "error", "code"?, "key"?}`: `error` is lower-case English for people, `code` is
what a program matches (see [Errors](#errors)). A connection is served in order: the reply to a request comes before
the game reads the next request on that connection. To run requests side by side (a long `join` next to `world` polls,
say), open another connection.

`ping` answers `protocol: 2`. A client finds what a game has by calling each op once without arguments: `unknown op`
means absent, `code:"disabled"` means present but turned off in the config, `ok` means present. The no-argument form of
every bedboo op is read-only.

## Choosing a mode

BeDD offers three ways to drive the game, all on at once. Pick them in this order:

1. **bedboo** (`defaultMode`): semantic ops on the game's own input pipeline and network session. `world` tells you
   where the player is and what screen shows; `look`, `input`, `slot`, `action` and `button` play; `send` and `stack`
   chat, run commands and move items; `join`, `settings` and `timescale` set up the session. Small JSON in and out, the same at any
   window size, and what bedboo uses.
2. **ui**: the game's screens as an accessibility-style tree (`ui`). Read a snapshot, then click, type into or scroll
   a node by its `ref`. Use it for menus and screens the bedboo ops do not reach.
3. **input**: what a person does with a keyboard and mouse (`key`, `text`, `mouse`, `raw`), plus `screenshot`. Use it
   last: a screenshot costs an agent far more tokens than a JSON snapshot, and pixel coordinates break when the layout
   changes.

`ping` lists the ops of each mode:

```json
{"ok": true, "protocol": 2, "pid": 23116, "defaultMode": "bedboo",
 "modes": {"bedboo": ["world","look","input","slot","action","button","settings","send","stack","timescale","join","tap"],
           "input": ["key","text","mouse","raw","release","flush","focus","window","screenshot"],
           "ui": ["ui","oreui"]},
 "hwnd": 4589122, "managed": true, "client": {"w": 1280, "h": 720}, "cursor": {"x": 640, "y": 360},
 "realForeground": false}
```

## System ops

| op | fields | reply |
|---|---|---|
| `ping` | | `protocol` (2), `pid`, `defaultMode`, `modes`, `hwnd`, `client{w,h}`, `cursor{x,y}`, `realForeground`, `managed` |
| `state` | | `ping`'s window fields + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput, loopbackRebinds, serverQueriesRefused, audio, tapLoopback, send, journal, sendsRefused}`, `clients` (WebSocket clients connected now), `hooks{name: installed}` (each game hook a feature installed; `false` while the feature is off), `mode` (ClientOnly\|ClientServer\|Remote, config `mode`), `connection{connection none\|local\|remote, inLevel, host, port}` (where the client plays) |
| `manager` | `action:"attach"` | `{ok}`: this connection is the game's manager |
| `close` | | `{ok}`, then the game closes |

**The manager.** The BeDD manager connects to every game and sends `{op:"manager", action:"attach"}`. While a
connection is attached, the game window stays off screen, off the taskbar and out of Alt+Tab. With config `manager`
(default `true`), a game that no manager attaches to within 30 s of its window existing, or whose attached connection
drops, closes itself. With `manager:false` the window stays hidden and the game never closes on its own (scripts that
run games without the manager). The label the manager shows comes from the environment variable `BEDD_LABEL` of the
game; `BEDD_RETURN_FOCUS` still hands the focus back after start.

## bedboo ops

Each op below is behind a config key, `true` by default; a key set to `false` makes the op reply
`{"ok":false,"code":"disabled","key":"<key>"}`.

| op | key | no-argument reply | action form |
|---|---|---|---|
| `world` | `gameHooks` | `inLevel`, `levelEpoch` (+1 per world joined), `topScreen` (e.g. `hud_screen`, `pause_screen`), `mouseGrabbed`, `showingMenu`, `inputMode` mouse\|touch\|gamepad\|undefined, `player{runtimeId, eye{x,y,z}, rot{yaw,pitch}, slot}` (null outside a world), `tick` (client ticks seen) | (none) |
| `look` | `gameHooks` | `{inLevel, yaw, pitch, tick}` | `yaw`, `pitch`, `timeoutMs` (1000\*, at most 60000) |
| `input` | `gameHooks` | `active`, `via` hooks\|keys, `hooks{raw,state}`, `tick`, `applied`, `ticksLeft` (a `ttlTicks` override) | `move{x,z}`, `jump`, `sneak`, `sprint`, `ttlMs` (300) or `ttlTicks` |
| `slot` | `gameHooks` | `{slot}` (null outside a world) | `slot` 0..8 |
| `action` | `gameHooks` | `{names}` | `name` drop\|dropAll\|inventory\|perspective\|pause\|swing; `hand` main\|off (swing) |
| `button` | `gameHooks` | `{held}` | `button` attack\|use\|pick; `action` down\|up\|click; `holdTicks`; `releaseOn` blockChange |
| `settings` | `gameHooks` | `{renderDistance, inLevel}` (chunks; null before the first world) | `renderDistance` (chunks, 1..255) |
| `send` | `allowSend` | `{allow}` (the ids in `sendAllow`) | `buf`, `timeoutMs` (2000\*, at most 10000) |
| `stack` | `allowStack` | `{enabled, via:"raw", stats{sent, answered, rejected, failed, resyncs}}` | `buf`, `timeoutMs` (2000\*); or `view:true` |
| `tap` | `tap` | `enabled`, `attached` (a subscriber is connected), `link{state up\|down, type remote\|local, address}`, `journal{frames, bytes, complete}` (the join journal now), `lastReplay` (what the last subscriber was replayed), `replay` (a journal is installed), `counters{in, out, dropped, sent, readers}`, `hooks` | `action:"subscribe"`: this connection receives the [tap](/reference/tap) as binary messages |
| `timescale` | (none) | see [below](#timescale-game-speed) | `scale`, `paused`, `timeoutMs` (2000) |
| `join` | (none) | `mode`, `connection` none\|local\|remote, `host`, `port`, `inLevel`, `joining`, `last` | `host` (null: leave the world), `port` (19132), `wait`, `timeoutMs` (60000) |

\* And `timeoutMs` (1000) of `slot`, `action` and `button`: these defaults hold at 20 ticks a second
and faster. When the client ticks slower (a slower game speed), a request without `timeoutMs`
waits that much longer (the default times the measured tick interval over 50 ms, up to the op's
maximum); a `timeoutMs` the caller passes is used as given. Likewise "the client is in a world"
(`not_in_level` of `slot`, `action`, `button`, `send`, `stack`) means a client tick within
20 tick intervals, at least 1 s.

- **`world`** is a snapshot the client thread takes: the screen fields every frame (a
  `ClientInstance::$update` post-hook, because a paused singleplayer world does not tick), the
  player every client tick. The connection's thread only copies it.
- **`look`** turns the player with `LocalPlayer::localPlayerTurn` on client ticks and replies
  `yaw`, `pitch`, `tick`, `turns` once both are within 0.5 degrees. Degrees as in
  player_auth_input: yaw 0 = +Z, 90 = -X; pitch -90 = up, clamped to +-89.9 like the game does.
  `localPlayerTurn(d)` turns 1:1 in degrees with the axes swapped (`{-dPitch, dYaw}`), and the turn
  lands after the tick that requested it. Errors: `not_in_level`, `timeout` (with the current
  `yaw`/`pitch`; a paused singleplayer world does not tick), `busy` (a newer `look` replaced it),
  `bad_args`.
- **`input`** moves the local player inside the game's own input pipeline, so the client builds
  every `player_auth_input` itself. `move` is player_auth_input's move vector (`x` = left - right,
  `z` = forward - back, each clamped to [-1, 1]; a diagonal is normalized). The override is a
  dead-man switch: it ends `ttlMs` (at most 60000) after the request, so a controller refreshes it
  while it wants to keep moving; `ttlMs:0`, `release` and the owning connection closing end it at
  once. `ttlTicks` (at most 1200; not with `ttlMs`, that is `bad_args`) ends it after that many
  client ticks instead, the same game time at any game speed; it also ends once the client stops
  ticking for 5 s (out of the world, paused), is `not_in_level` when asked for while it is not
  ticking, and the probe reports the ticks left as `ticksLeft`.
  - `via:"hooks"`: post-hooks on `ClientInputUpdateSystem::extractRawHIDInput` (writes
    `RawMoveInputComponent::mRawInput`/`mRawMove`) and `inputHandlerUpdatePlayerState` (writes
    `MoveInputComponent::mInputState`/`mRawInputState` before and after the original:
    `MoveInputState` flags `SneakDown`=0, `JumpDown`=7, `SprintDown`=8, `Up`..`Right`=13..16 and
    the jump/sneak input edges 21..26, plus `mAnalogMoveVector`). Applied only while the mouse is
    grabbed and no screen shows; the key bindings do not matter. The keyboard handler changes those
    flags only on key events, so on the first tick without the override they are cleared once.
    `tick` counts `inputHandlerUpdatePlayerState` calls, `applied` the ticks the override was written.
  - `via:"keys"` (a hook did not install): the default keymap's virtual keys (W/A/S/D, SPACE,
    LSHIFT, LCTRL) are held until the deadline. They go through the window like `key`, so they
    also reach an open screen, and a rebound key breaks them.
- **`slot`, `action`, `button`** are queued and answered once the next client tick applied them
  (`timeoutMs`, default 1000): `{"ok":true,"tick":n,...}`, or `code` `not_in_level`, `busy` (a
  screen is open; `button up` still works), `timeout`, `bad_args`, `refused`. A paused singleplayer
  world does not tick: after `action pause` there, resume with the `key` op (ESC).
  - `slot`: the hotbar key `1`..`9` of the default key map, replied once the player holds the
    slot. The client sends `mob_equipment` itself. Replies `slot` and `via` keys\|none (already held).
  - `action`: the key of the default key map, released on the next tick: drop Q, dropAll Ctrl+Q,
    inventory E, pause Escape; perspective calls `ClientInputCallbacks::handleTogglePerspectiveButtonPress`
    (since 1.26.60 the game exports none of the other `handle*ButtonPress`). A rebound key breaks them.
    `swing` calls `LocalPlayer::swing`: the arm swings with no attack and the client sends the animate
    packet itself; it works with a screen open too. `hand:"off"` swings the off hand on this client
    only (animate carries no hand). Replies `name`, `hand`.
  - `button`: left/right/middle on the virtual GameInput mouse, released
    by the tick listener after `holdTicks` (click: default 2) or, with `releaseOn:"blockChange"`,
    once the block the client's hit result pointed at when it went down changes, turns to air or is
    no longer targeted (`refused` when no block is targeted). `up` applies at once. Never blocks
    the connection. Replies `held`, and `block{x,y,z}`.
- **`settings`**: `renderDistance` (chunks) sets the video option "Render Distance" of every renderer
  (`ViewDistance`, `DeferredViewDistance` for Vibrant Visuals, `RayTracingViewDistance`), each kept to its own
  values by the game and not saved to options.txt, on the next client tick, then calls
  `ClientInstance::updateChunkRadius`: the client sends request_chunk_radius itself and the server answers with the
  radius it grants (chunk_radius_update on the tap, which may be larger than asked). Replies `renderDistance` (what
  the client renders now), `requested`, `tick`. Codes: `not_in_level`, `timeout`, `bad_args`, `unavailable`.
- **`send`**: `buf` is the base64 of one serverbound packet (varint header + body, as
  bedrock-protocol serializes it). It is rebuilt on the client thread
  (`MinecraftPackets::createPacket` + `Packet::read` with the client NetworkSystem's
  `ReflectionCtx`) and sent through the local player (`sendNetworkPacket`), on a remote server and
  in singleplayer alike; the reply `{packetId, name}` comes once it is sent. Bytes the game does not
  read are `bad_args` and nothing is sent; an id not in `sendAllow` is `refused`. `state` reports
  `stats.send{sent, refused, failed, lastError}`.
- **`join`** makes the client join a server by address through the game's own external-server join
  (`ClientInstance::connectToThirdPartyServer`), from the title screen or from inside a world, which it leaves first.
  Since 1.26.60 it answers the two prompts in the way, as a player would with the keyboard: "Online play is not rated"
  (Proceed, then it connects again) and "Trust this server?" for the server it joins (Trust and Join). Replies
  `started:true` once it connects, with `wait:true` `joined:true` once it is in the level; `left`: a world was left
  first. Codes: `join_failed` (the client gave up and shows why), `timeout` (`screen`: the game may still be
  connecting), `busy` (a join is in progress), `not_allowed` (a host off the PC that `loopbackOnly` keeps the game
  from: not in `allowHosts`), `stopped`, `no_client`, `unavailable`, `bad_args`. A server off the PC needs Remote
  mode (config `allowHosts`).
  `host: null` only leaves: the client leaves the world it is in (`requestLeaveGame`, as Save & Quit or Disconnect
  in the pause menu) and goes back to its menus. Replies `left:true` (`started:true` without `wait`; with `wait`,
  once the world is saved and no progress screen shows), or `left:false` when it was in no world; `busy` while a
  join or leave is in progress.
- **`tap`**: `{op:"tap", action:"subscribe"}` answers `{ok:true}`, and from then on the connection receives one binary
  message per tap frame: HELLO, the join journal's replay, then the live stream. One subscriber at a time (another
  gets `busy`); a subscriber more than 32 MiB behind gets DROPPED and its connection is closed. Send nothing else on a
  subscribed connection. The frames are described in [Tap protocol](/reference/tap).

### `stack`: item stack requests through the client

| Form | Reply |
|---|---|
| `{op:"stack"}` (probe) | `{ok, enabled:true, via:"raw", stats:{sent, answered, rejected, failed, resyncs}}` |
| `{op:"stack", buf, timeoutMs=2000}` | `{ok, via:"raw", requestIds:[client ids], map:{"<caller id>": client id}}` |
| `{op:"stack", view:true}` | `{ok, inventory:[...], ui:[...], open?:{containerId, type, items:[...]}}`: what the client shows, each `{slot, item, count, stackId}` |

- `buf` is the base64 of one whole item_stack_request packet (id 147), varint header plus body, as
  bedrock-protocol serializes it. Any other packet id, and bytes the game does not read, are `bad_args`.
- On the next client tick the packet is rebuilt with `MinecraftPackets::createPacket` and
  `Packet::read`, and the local player sends it.
- Request ids: each request gets a new client id, and the reply comes back once the packet is sent. Ids
  are odd, negative and counted down from `-0x40000001`, far below the client's own `-1, -3, ...`.
  Every slot stack id that names a caller id is renamed too. That covers the request's own id (a
  `consume`, or the crafted output) and an earlier request of the same packet or of an earlier one
  (the last 256 ids are remembered). `map` keys are the caller's ids as strings.
- The server answers with the client ids in item_stack_response, which bedboo sees on the tap. The
  answer can only arrive after the packet is sent, but it can come before the reply to `stack` is
  read (it is on another connection), so the caller holds an unmatched response until the reply arrives.
- `via:"raw"`: the request does not go through the client's `ItemStackNetManagerClient`. In this game version
  the manager has no callable symbol (every member is MCNAPI in LeviLamina's headers), and its
  predictions come from the screen controllers moving items, not from request actions. So the
  client predicts nothing for these requests.
- Resync: when the server accepts one of these requests (hooked
  `ClientNetworkHandler::handle(ItemStackResponsePacket)`), the client sends
  `inventory_transaction {type: inventory_mismatch}` on its next tick. That is the packet the client
  itself sends when its inventory is out of step, and the server answers with the whole inventory.
  The client then shows the server's state, open container included. A rejected request changes
  nothing on either side, so it gets no resync.
- The server checks every request (stack ids, open screen, recipe). A screen the request needs (the
  inventory, a chest) must already be open in the game: open it with `action inventory`, `key E` or
  by using the block.

### `timescale`: game speed

Not behind a config key. It changes the speed of a world this game hosts (singleplayer, or a LAN
world it hosts): the client and its local server, together.

| Form | Reply |
|---|---|
| `{op:"timescale"}` (probe) | `{ok, scale, paused, source, tps:{client, server}, ticksPerSecond, effective:{client, server}, local, inLevel}` |
| `{op:"timescale", scale?, paused?, timeoutMs=2000}` | the probe's fields, once the client and the local server both run at it |

- `scale` is clamped to 0.05..20 (1 = normal, 20 ticks a second). `paused:true` stops the ticks of both sides
  (`Minecraft::setSimTimePause`) and `false` resumes them. The game's own pause menu pauses and resumes the same
  way.
- `source`: `default` (1, nothing set), `config` (`timeScale`, applied on entering the world), `local` (set through
  this API), `server` (a server running BeDD set it).
- `tps.client`, `tps.server`: ticks of the client and of the local server per second, measured over the last 2 s (so
  they trail a change by up to 2 s; 0 while paused). `tps.server` is `null` on a remote server. `ticksPerSecond`: the
  client timer's base rate (20). `effective.client` / `effective.server`: `{scale, paused, ticksPerSecond}` as each
  side's Minecraft reported them after it last applied a change (`null` before the first).
- Each side is changed on its own thread: the server's through `ServerInstance::queueForServerThread`, the client's
  after the next `ClientInstance::update` (frames go on while the ticks are paused). The scale is written to the sim
  timer (`Timer::mTimeScale`) directly: `Minecraft::setSimTimeScale` multiplies the current scale and caps it at 10,
  so it is not idempotent. A timer catches up only a few ticks per frame, so `tps` shows what the game achieves. With
  a local world, client and server alike: 0.05 → 1, 0.5 → 10, 2 → 40, 3 → 60, 5 → 100, 10 → 200, 20 → 400 ticks a
  second; the same scale set again changes nothing.
- Leaving a world puts the client back to 1; entering one applies `timeScale` (default 1), so nothing of the last
  world carries over.
- Errors: `not_in_level`; `remote` (the world is on a server this game does not host: its speed cannot change from the
  client); `server_led` (that server runs BeDD and has sent its scale: change it there); `timeout` (a side did not
  apply it in time: it still does once its thread runs, and `effective` shows it); `refused`; `bad_args`.
- Server-led: a server running BeDD sends each player a level_event packet (`event_id` 3007 SimTimeScale, position
  0,0,0, `data` = the scale's float bits as int32, 0 while the server is paused) when its scale or pause changes and
  when the player joins. The client applies it (hooked `ClientNetworkHandler::handle(LevelEventPacket)`, after the
  game's own handling, and only where the game did not already reach that scale); 0 pauses the client, keeping its
  scale for the resume.

### Who sends what (bedboo transport `bedd`)

The real client owns every serverbound packet; bedboo turns what it would have sent into intents.
This table is the same as bedboo's `lib/bedd/authority.js`.

| Class | Packets | Handling |
|---|---|---|
| clientOwned | login, client_to_server_handshake, request_network_settings, resource_pack_client_response, client_cache_status, client_cache_blob_status, request_chunk_radius, subchunk_request, serverbound_loading_screen, set_local_player_as_initialized, network_stack_latency, tick_sync, player_skin, emote_list, respawn, show_credits, player_action (respawn / dimension_change_ack / start_item_use_on / stop_item_use_on), animate, interact mouse_over_entity | bedboo drops them (counted): the client sends its own. What bedboo wants from them goes through an op: `bot.swingArm` is `action swing` (animate), a view distance is `settings renderDistance` (request_chunk_radius), Respawn is clicked through `ui`, and the End credits are left with ESC (the client then sends show_credits) |
| translated | player_auth_input (move_vector + flags to `input`; yaw/pitch to `look`; block_actions start_break/abort_break/stop_break to `button attack`), mob_equipment (`slot`), inventory_transaction (item_use click_block/click_air, item_release, item_use_on_entity: `look`, then `button use`/`attack`), interact open_inventory (`action inventory`), container_close (key ESC), item_stack_request (`stack`), block_pick_request (`look`, then `button pick`) | bedboo's router turns them into ops |
| shared | text, command_request | sent through `send`; fallback: T or SLASH, then `text`, then ENTER |
| screens | player_action stop_sleeping (Leave Bed), a sign's block_entity_data (the sign editor) | bedboo works the client's own screen through `ui` |
| allowlisted | any other packet whose id the `send` probe lists in `allow` (config `sendAllow`; by default book_edit 97 and command_block_update 78, which the client sends from its book and command block screens) | sent as is through `send` |
| unsupported | everything else (crafting_event, set_default_game_type, ...) | bedboo throws BeddUnsupportedError (ERR_BEDD_UNSUPPORTED) |

item_stack_request is unsupported only when the `stack` probe does not answer `ok`. `bot.quit()` leaves the world with
`join {host: null}`; `createBot({host, port})` joins with `join`.

## ui ops

Config key `ui` (default `true`). `ui` reads the game's screens as a tree, the way an accessibility API exposes a
desktop app (issue [#21](https://github.com/LiteLDev/BeDD/issues/21)), and acts on a node by its `ref`.

| Form | Reply |
|---|---|
| `{op:"ui", depth?, scope?, includeInvisible?, maxNodes?}` | `{snapshot, screens, active, roots, nodes{ref: node}, truncated}` |
| `{op:"ui", snapshot, action:"click", ref}` | `{ok}` once the click was dispatched |
| `{op:"ui", snapshot, action:"setText", ref, text}` | `{ok}`: the text box holds `text` |
| `{op:"ui", snapshot, action:"scroll", ref, notches}` | `{ok}` |
| `{op:"oreui", url?, maxNodes?, maxText?, timeoutMs?, force?}` | `{url, title, width, height, nodes[], truncated, findings}`: the DOM of the Ore UI view |

- A node is `{role, name, text, value, state, bounds, boundsNorm, actions, children, source, hash}`: `bounds` in client
  pixels, `boundsNorm` in fractions of the client area, `actions` what the node accepts, `children` refs, `hash`
  changes when the node does.
- `screens` is the scene stack, `active` the screen on top, `roots` the refs of its root nodes. `scope` reads below
  one ref only; `depth` and `maxNodes` cut the tree (`truncated:true`); hidden controls are left out unless
  `includeInvisible`.
- JSON UI screens are read control by control (SceneStack → `UIScene::mScreenView` → `VisualTree::mRootControl` →
  `UIControl::mChildren`/`mComponents`). An OreUI screen is one opaque node `{role:"group", source:{kind:"oreui", url}}`:
  `oreui` reads inside it: the view's DOM as a node array (`tag`, `id`, `class`, `testId`, `role`, `name`, `text`,
  `value`, `state`, `bounds`, `children` as indexes); `url` waits for a view whose URL contains it. Ore UI nodes
  have no refs: click one with `mouse` at the centre of its `bounds`. See [UI tree](./ui#ore-ui-screens-oreui).
- Actions go through the same virtual mouse and keyboard as the input ops, so they need nothing the game would not
  accept from a player. `mouse` also takes `ref` + `snapshot` instead of a point: it aims at the centre of the node.
- Errors: `stale_ref` (the ref is not in that snapshot, or its node is gone), `stale_snapshot` (the screens changed:
  read the tree again), `occluded` (another control covers the node), `not_ready` (no tree yet: the game is loading),
  `unsupported` (the node cannot do that action).

```json
{"id": 20, "op": "ui", "depth": 6}
{"id": 20, "ok": true, "snapshot": "s17", "screens": ["start_screen"], "active": "start_screen", "roots": ["n1"],
 "truncated": false,
 "nodes": {"n1": {"role": "screen", "name": "start_screen", "children": ["n42"], "actions": [], "hash": "9f1c"},
           "n42": {"role": "button", "name": "play_button", "text": "Play", "state": {"visible": true, "enabled": true},
                   "bounds": {"x": 540, "y": 330, "w": 200, "h": 40},
                   "boundsNorm": {"x": 0.42, "y": 0.46, "w": 0.16, "h": 0.06},
                   "actions": ["click"], "children": [], "source": {"kind": "jsonui"}, "hash": "03ab"}}}
{"id": 21, "op": "ui", "snapshot": "s17", "action": "click", "ref": "n42"}
```

## input ops

Not behind a config key. These ops reply after the game thread dispatched the input (`timeoutMs`, default 5000).

| op | fields | |
|---|---|---|
| `key` | `key` (name) or `vk`; `action` press\|down\|up; `holdMs` (60) | names: letters, digits, ESC, ENTER, BACK, TAB, SPACE, SHIFT, CTRL, ALT, LEFT/UP/RIGHT/DOWN, HOME, END, PGUP, PGDN, INSERT, DELETE, F1..F24, SLASH, GRAVE, MINUS, EQUALS |
| `text` | `text`; `delayMs` | one `WM_CHAR` per UTF-16 unit |
| `mouse` | `action` move\|down\|up\|click\|wheel; `x`,`y` (client px), `rx`,`ry` (fractions) or `ref` + `snapshot` (a ui node); `button` left\|right\|middle; `notches`; `holdMs` | moves the virtual cursor (if a point is given), then buttons/wheel on the virtual GameInput mouse |
| `raw` | `action` move\|down\|up\|click\|wheel; `dx`,`dy` (mouse counts); `steps`; `button`; `notches`; `holdMs` | virtual GameInput mouse: camera while the mouse is grabbed |
| `release` | | releases every held key and button, whoever pressed it, and ends the `input` move override |
| `focus` | | re-sends the "focus gained" messages |
| `window` | `clientWidth`, `clientHeight` | never activates the window; it stays off screen |
| `flush` | | waits until everything posted was dispatched |
| `screenshot` | `format` png\|jpeg; `quality` (jpeg); `maxWidth` (960); `region{x,y,w,h}` (client px) | `{width, height, format, data}`, `data` the base64 image. PrintWindow (`PW_RENDERFULLCONTENT`) of the game window, encoded with WIC: works while the window is covered or off screen |

## Errors

| code | meaning |
|---|---|
| `disabled` | the op's config key is `false` (named in `key`), or BeDD is unloading |
| `not_in_level` | the client is not in a world (on the server build: the server has not started) |
| `refused` | the game refused it (an id outside `sendAllow`, no block targeted for `releaseOn`, ...) |
| `timeout` | the game did not get there within `timeoutMs` (it may still) |
| `bad_args` | missing or invalid fields, or bytes the game cannot read |
| `busy` | something else holds it: a screen is open, a newer `look`, a join in progress, the tap's subscriber |
| `stopped` | BeDD is unloading |
| `join_failed` | the client gave up joining and shows why |
| `not_allowed` | a host off the PC that `loopbackOnly` keeps the game from (not in `allowHosts`) |
| `unavailable` | a game hook the op needs did not install (see the game log) |
| `no_client` | the client instance does not exist yet |
| `remote` | the world is on a server this game does not host |
| `server_led` | that server runs BeDD and leads the game speed |
| `stale_ref` | the `ref` is not in the snapshot, or its node is gone |
| `stale_snapshot` | the screens changed since the snapshot |
| `occluded` | another control covers the node |
| `not_ready` | no UI tree yet |
| `unsupported` | the node cannot do that action |

An error without `code` is a request the game could not run at all: `unknown op 'x'` (the op does not exist in this
build), a message that is not a JSON object, or input the game thread did not process within `timeoutMs`.

## Several clients

Several connections can be open at once (config `wsMaxClients`, default 8), each served by its own thread and in
order. Ops that post window input are serialized across connections, so two clients never interleave a press or a
typed text; `ping`, `state`, `tap`, `world`, `look`, `input`, `settings`, `send`, `stack`, `timescale`, `join` and `ui` reads run
without that lock (they only read, or hand their work to the game threads). What a connection holds is released when it
closes (or its process dies), and nothing another connection holds: its keys and mouse buttons, the `button` holds it
made and the `input` override it set.

For units with per-client state (`src/BeDD.h`): `currentClient()` is the id of the connection the current thread serves
(from 1, never reused; 0 on the game thread). Tag the state with it and release it from `clientGone()` in
`src/Pipe.cpp` (as `clearMoveInputOf` and `releaseGameButtonsOf` do), or register a callback with `onPipeClientGone(fn)`
in `enable()`: either runs on the departing connection's thread, after its keys and buttons are released, with input
ops serialized (not at shutdown).

## Security

The token is the boundary: anything that can read the discovery file (the current user's processes) can drive the
game, and nothing else can. The server listens on `127.0.0.1` only, refuses browsers through the `Origin` rule and
compares the token in constant time.

What a holder of the token can do is wide, because every feature key defaults to `true`:

- `send` makes the client send packets as the signed-in player, limited to `sendAllow` (default text 9 and
  command_request 77: chat and commands, as the player could type them). Ids 1 (login), 30 (inventory_transaction),
  144 (player_auth_input) and 147 (item_stack_request) are always refused, even when listed: the client owns them,
  and forged ones would fight its prediction and bookkeeping. Widen `sendAllow` only for a server you control.
- `stack` can move, craft, drop and destroy items of the signed-in player on whatever server the client is on.
- The tap carries the whole session of the signed-in player.

Set `allowSend`, `allowStack` or `tap` to `false` when a harness does not need them, and never share a token. Add an
origin to `allowOrigins` only for a local page you trust.

## Server target

BeDD built with `--target_type=server` runs inside a Bedrock Dedicated Server with LeviLamina and serves the same
WebSocket (discovery file with `kind:"server"`), with the same envelope. The server leads the game speed: it
broadcasts its scale to every player as a LevelEvent packet (`mEventId` 3007, SimTimeScale; `mPos` 0,0,0; `mData` the
bits of the float scale, 0 while paused), whenever it or the pause changes and to each player once it has spawned;
BeDD on a client follows it. Other ops answer `unknown op`.

| op | fields | reply |
|---|---|---|
| `ping` | | `protocol`, `pid`, `target:"server"` (no `hwnd`) |
| `state` | | `target:"server"`, `clients`, `loopbackOnly`, `stats{loopbackRebinds, serverQueriesRefused}` |
| `server` | | `tick` (the level's current tick), `tps` (ticks measured over the last 2 s), `paused`, `port` (`server-port`), `players[{name, xuid}]` |
| `timescale` | none: probe | `scale`, `paused`, `source` default\|config\|local, `tps{client:null, server}` (measured), `ticksPerSecond` (what the scale asks for: 20 × scale, 0 while paused) |
| `timescale` | `scale` 0.05..20 and/or `paused` true\|false; `timeoutMs` (5000) | as the probe, after the change. Runs on the server thread (`ServerInstance::queueForServerThread`, which also runs while paused): the scale goes straight into the simulation timer (`Minecraft::setSimTimeScale` multiplies and caps at 10), the pause through `setSimTimePause` |
| `command` | `command` (a leading `/` is optional); `timeoutMs` (10000) | `success`, `successCount`, `output[]` (the translated lines), run as the server console would |

Errors: `not_in_level` (the server has not started), `bad_args`, `timeout` (the server thread did
not answer). `tps.server` is what the server achieves: at a high scale it may stay below
`ticksPerSecond` when a tick takes longer than the scale allows.
