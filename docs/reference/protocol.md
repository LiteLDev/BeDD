# Control pipe protocol

The PowerShell client (`BeDD.psm1`) is one client of this protocol; anything that can open a named pipe can talk to
the game the same way. `Invoke-BeDD @{ op = '...' }` sends a request as it is.

## Requests and replies

`\\.\pipe\<pipeName>` (local only, default `BeDD`), UTF-8, one JSON object per line
each way. An `id` is echoed. Replies are `{"ok":true,...}` or `{"ok":false,"error":"..."}`. Input
ops reply after the game thread has dispatched the input (`timeoutMs`, default 5000). The protocol
version (`ping`.`protocol`) stays 1: newer ops are found by calling them (below).

| op | fields | |
|---|---|---|
| `ping` | | `protocol`, `pid`, `hwnd`, `client{w,h}`, `cursor{x,y}`, `realForeground`, `managed` |
| `state` | | `ping` fields + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput, loopbackRebinds, serverQueriesRefused, audio, tapLoopback, send, journal, sendsRefused}`, `clients` (control-pipe clients connected now), `hooks{name: installed}` (each game hook a feature installed; `false` while the feature is off), `mode` (ClientOnly\|ClientServer\|Remote, config `mode`), `connection{connection none\|local\|remote, inLevel, host, port}` (where the client plays) |
| `key` | `key` (name) or `vk`; `action` press\|down\|up; `holdMs` | names: letters, digits, ESC, ENTER, BACK, TAB, SPACE, SHIFT, CTRL, ALT, LEFT/UP/RIGHT/DOWN, HOME, END, PGUP, PGDN, INSERT, DELETE, F1..F24, SLASH, GRAVE, MINUS, EQUALS |
| `text` | `text`; `delayMs` | one `WM_CHAR` per UTF-16 unit |
| `mouse` | `action` move\|down\|up\|click\|wheel; `x`,`y` (client px) or `rx`,`ry` (fractions); `button` left\|right\|middle; `notches`; `holdMs` | moves the virtual cursor (if a point is given), then buttons/wheel on the virtual GameInput mouse |
| `raw` | `action` move\|down\|up\|click\|wheel; `dx`,`dy` (mouse counts); `steps`; `button`; `notches`; `holdMs` | virtual GameInput mouse: camera while the mouse is grabbed |
| `release` | | releases every held key and button, whoever pressed it, and ends the `input` move override |
| `focus` | | re-sends the "focus gained" messages |
| `window` | `clientWidth`, `clientHeight` | never activates the window; it stays off screen |
| `flush` | | waits until everything posted was dispatched |
| `join` | `host`, `port` (19132), `wait`, `timeoutMs` (60000) | the client joins a server by address through the game's own external-server join (`ClientInstance::connectToThirdPartyServer`), from the title screen or from inside a world, which it leaves first. Replies `started:true` once it connects, with `wait:true` `joined:true` once it is in the level; `left`: a world was left first. Codes: `join_failed` (the client gave up and shows why), `timeout` (`screen`: the game may still be connecting), `busy` (a join is in progress), `not_allowed` (a host off the PC that `loopbackOnly` keeps the game from: not in `allowHosts`), `bad_args`. Without `host`: `mode`, `connection` none\|local\|remote, `host`, `port`, `inLevel`, `joining` (the attempt in progress), `last` (how the last one ended). A server off the PC needs Remote mode (config `allowHosts`) |

## Several clients

Several clients can be connected at once (config `pipeInstances`, default 4; 1 = one at a time),
each served by its own thread. Ops that post window input are serialized, so two clients never
interleave a press or a typed text; `ping`, `state`, `tap`, `world`, `look`, `input`, `send`,
`stack`, `timescale` and `join` run without that lock (they only read, or hand their work to the game threads). What a
client holds is released when it disconnects (or its process dies), and nothing another client
holds: its keys and mouse buttons, the `button` holds it made and the `input` override it set. Ops
the manager window forwards over the manager link are neither serialized nor owned.

For units with per-client state (`src/BeDD.h`): `currentPipeClient()` is the id of the client the
current thread serves (from 1, never reused; 0 on the manager link and the game thread). Tag the
state with it and release it from `clientGone()` in `src/Pipe.cpp` (as `clearMoveInputOf` and
`releaseGameButtonsOf` do), or register `onPipeClientGone(fn)` in `enable()`: either runs on the
departing client's thread, after its keys and buttons are released, with input ops serialized
(not at shutdown).

## Ops for bedboo (transport `bedd`)

Each of these is off unless its config key is on, and then replies
`{"ok":false,"code":"disabled","key":"<key>"}`. Errors may carry `code`: `disabled`,
`not_in_level`, `refused`, `timeout`, `bad_args`, `busy`. An op this build lacks answers
`unknown op 'x'`. A client finds what a game has by calling each op once without arguments:
`unknown op` = absent, `code:"disabled"` = present but off, `ok` = present. The no-argument form
of every op is read-only.

| op | key | no-argument reply | action form |
|---|---|---|---|
| `tap` | `tap` | `enabled`, `pipe`, `attached` (a reader is connected), `link{state up\|down, type remote\|local, address}`, `journal{frames, bytes, complete}` (the join journal now), `lastReplay` (what the last reader was replayed), `replay` (a journal is installed), `counters{in, out, dropped, sent, readers}`, `hooks` | (none) |
| `world` | `gameHooks` | `inLevel`, `levelEpoch` (+1 per world joined), `topScreen` (e.g. `hud_screen`, `pause_screen`), `mouseGrabbed`, `showingMenu`, `inputMode` mouse\|touch\|gamepad\|undefined, `player{runtimeId, eye{x,y,z}, rot{yaw,pitch}, slot}` (null outside a world), `tick` (client ticks seen) | (none) |
| `look` | `gameHooks` | `{inLevel, yaw, pitch, tick}` | `yaw`, `pitch`, `timeoutMs` (1000\*, at most 60000) |
| `input` | `gameHooks` | `active`, `via` hooks\|keys, `hooks{raw,state}`, `tick`, `applied`, `ticksLeft` (a `ttlTicks` override) | `move{x,z}`, `jump`, `sneak`, `sprint`, `ttlMs` (300) or `ttlTicks` |
| `slot` | `gameHooks` | `{slot}` (null outside a world) | `slot` 0..8 |
| `action` | `gameHooks` | `{names}` | `name` drop\|dropAll\|inventory\|perspective\|pause |
| `button` | `gameHooks` | `{held}` | `button` attack\|use\|pick; `action` down\|up\|click; `holdTicks`; `releaseOn` blockChange |
| `send` | `allowSend` | `{allow}` (the ids in `sendAllow`) | `buf`, `timeoutMs` (2000\*, at most 10000) |
| `stack` | `allowStack` | `{enabled, via:"raw", stats{sent, answered, rejected, failed, resyncs}}` | `buf`, `timeoutMs` (2000\*); or `view:true` |

\* And `timeoutMs` (1000) of `slot`, `action` and `button`: these defaults hold at 20 ticks a second
and faster. When the client ticks slower (a slower game speed), a request without `timeoutMs`
waits that much longer (the default times the measured tick interval over 50 ms, up to the op's
maximum); a `timeoutMs` the caller passes is used as given. Likewise "the client is in a world"
(`not_in_level` of `slot`, `action`, `button`, `send`, `stack`) means a client tick within
20 tick intervals, at least 1 s.

- **`world`** is a snapshot the client thread takes: the screen fields every frame (a
  `ClientInstance::$update` post-hook, because a paused singleplayer world does not tick), the
  player every client tick. The pipe thread only copies it.
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
  while it wants to keep moving; `ttlMs:0`, `release` and the owning client disconnecting end it at
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
- **`slot`, `action`, `button`**: the pipe thread queues them and replies once the next client
  tick applied them (`timeoutMs`, default 1000): `{"ok":true,"tick":n,...}`, or `code`
  `not_in_level`, `busy` (a screen is open; `button up` still works), `timeout`,
  `bad_args`, `refused`. A paused singleplayer world does not tick: after `action pause` there,
  resume with the `key` op (ESC).
  - `slot`: the hotbar key `1`..`9` of the default key map, replied once the player holds the
    slot. The client sends `mob_equipment` itself. Replies `slot` and `via` keys\|none (already held).
  - `action`: the key of the default key map, released on the next tick: drop Q, dropAll Ctrl+Q,
    inventory E, pause Escape; perspective calls `ClientInputCallbacks::handleTogglePerspectiveButtonPress`
    (since 1.26.60 the game exports none of the other `handle*ButtonPress`). A rebound key breaks them.
  - `button`: left/right/middle on the virtual GameInput mouse, released
    by the tick listener after `holdTicks` (click: default 2) or, with `releaseOn:"blockChange"`,
    once the block the client's hit result pointed at when it went down changes, turns to air or is
    no longer targeted (`refused` when no block is targeted). `up` applies at once. Never blocks
    the pipe. Replies `held`, and `block{x,y,z}`.
- **`send`**: `buf` is the base64 of one serverbound packet (varint header + body, as
  bedrock-protocol serializes it). It is rebuilt on the client thread
  (`MinecraftPackets::createPacket` + `Packet::read` with the client NetworkSystem's
  `ReflectionCtx`) and sent through the local player (`sendNetworkPacket`), on a remote server and
  in singleplayer alike; the reply `{packetId, name}` comes once it is sent. Bytes the game does not
  read are `bad_args` and nothing is sent; an id not in `sendAllow` is `refused`. `state` reports
  `stats.send{sent, refused, failed, lastError}`.
- **`stack`**: see below.

## Security

`send` lets any process of the current user that can open the pipe make the client
send packets as the signed-in player, so it is off by default (`allowSend`) and limited to
`sendAllow` (default text 9 and command_request 77: chat and commands, as the player could type
them). Ids 1 (login), 30 (inventory_transaction), 144 (player_auth_input) and 147
(item_stack_request) are always refused, even when listed: the client owns them, and forged ones
would fight its prediction and bookkeeping. Widen `sendAllow` only for a server you control. While
`allowStack` is on, anything that can open the pipe can move, craft, drop and destroy items of the
signed-in player on whatever server the client is on. The tap carries the whole session of the
signed-in player. Keep all three off unless a harness needs them; the manager pipe never turns
them on.

## `stack`: item stack requests through the client (`allowStack`)

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
  read, so the caller holds an unmatched response until the reply arrives.
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

## `timescale`: game speed

Not behind a config key. It changes the speed of a world this game hosts (singleplayer, or a LAN
world it hosts): the client and its local server, together.

| Form | Reply |
|---|---|
| `{op:"timescale"}` (probe) | `{ok, scale, paused, source, tps:{client, server}, ticksPerSecond, effective:{client, server}, local, inLevel}` |
| `{op:"timescale", scale?, paused?, timeoutMs=2000}` | the probe's fields, once the client and the local server both run at it |

- `scale` is clamped to 0.05..20 (1 = normal, 20 ticks a second). `paused:true` stops the ticks of both sides
  (`Minecraft::setSimTimePause`) and `false` resumes them. The game's own pause menu pauses and resumes the same
  way.
- `source`: `default` (1, nothing set), `config` (`timeScale`, applied on entering the world), `local` (set through the
  pipe), `server` (a server running BeDD set it).
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

## Who sends what (bedboo transport `bedd`)

The real client owns every serverbound packet; bedboo turns what it would have sent into intents.
This table is the same as bedboo's `lib/bedd/authority.js`.

| Class | Packets | Handling |
|---|---|---|
| clientOwned | login, client_to_server_handshake, request_network_settings, resource_pack_client_response, client_cache_status, client_cache_blob_status, request_chunk_radius, subchunk_request, serverbound_loading_screen, set_local_player_as_initialized, network_stack_latency, tick_sync, player_skin, emote_list, respawn, player_action (respawn / dimension_change_ack / start_item_use_on / stop_item_use_on), animate, interact mouse_over_entity | bedboo drops them (counted): the client sends its own. A respawn is the user clicking Respawn on the client |
| translated | player_auth_input (move_vector + flags to `input`; yaw/pitch to `look`; block_actions start_break/abort_break/stop_break to `button attack`), mob_equipment (`slot`), inventory_transaction (item_use click_block/click_air, item_release, item_use_on_entity: `look`, then `button use`/`attack`), interact open_inventory (`action inventory`), container_close (key ESC), item_stack_request (`stack`) | bedboo's router turns them into ops |
| shared | text, command_request | sent through `send`; fallback: T or SLASH, then `text`, then ENTER |
| unsupported | book_edit, block_pick_request, crafting_event, set_default_game_type, everything else | bedboo throws BeddUnsupportedError (ERR_BEDD_UNSUPPORTED) |

item_stack_request is unsupported only when the `stack` probe does not answer `ok`.

## Server target (client+server mode)

BeDD built with `--target_type=server` runs inside a Bedrock Dedicated Server with LeviLamina and
serves `\\.\pipe\<pipeName>` (default `BeDD-Server`) with the same framing. The server leads the
game speed: it broadcasts its scale to every player as a LevelEvent packet (`mEventId` 3007,
SimTimeScale; `mPos` 0,0,0; `mData` the bits of the float scale, 0 while paused), whenever it or
the pause changes and to each player once it has spawned; BeDD on a client follows it. Other ops answer `unknown op`.

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

## Manager pipe

`\\.\pipe\BeDD-Manager-<session id>` (current user only), served by
`BeDDManager.ps1`, JSON lines like the control pipe.
`BEDD_MANAGER_PIPE` names another manager's pipe instead (scripts and games read it).

- **Clients** (one request per connection): `ping` (`games`; `ok` is false while it closes),
  `adopt` `{pid, instance, label}` (`Start-BeDDGame.ps1` right after starting a game), `list`
  (`games[]`: `pid`, `instance`, `label`, `pipe`, `hwnd`, `linked`, `managed`, `job`,
  `thumbnail`), `game` `{pid, action: close}`, `shutdown`.
- **Games** (the mod): the first line is `hello` `{protocol, pid, hwnd, pipe, instance, label}`
  (`label` from `BEDD_LABEL`), answered with `{"ok":true}` (a manager that is closing hangs up
  instead; the game is linked only once welcomed). The connection stays open and the manager sends
  requests on it: every control-pipe op (it polls `state` every second), plus `close` (`WM_CLOSE` to
  the game window). A game with no manager starts one (config `managerScript`); one that no manager
  links within 90 s, or that loses the link for good, closes itself.

