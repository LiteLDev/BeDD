# Contributing to Be.D.D.

Thanks for helping! Bug reports, ideas and pull requests are all welcome.

## Reporting bugs and requesting features

Use the [issue templates](https://github.com/LiteLDev/BeDD/issues/new/choose). For bugs, include:

- the Be.D.D., LeviLamina and Minecraft versions;
- what the script sent (the JSON request or the PowerShell call) and what the game did instead;
- `mods/BeDD/config/config.json`;
- the relevant part of `logs/latest.log`, ideally with `"trace": true` (it shows which input reached the game and whether it was synthetic or real).

## Development setup

See [Development](#development) below. The code is small:

| File | |
|---|---|
| `src/BeDD.h` | shared state and declarations of the parts below |
| `src/Mod.cpp` | entry point, config, virtual key/cursor state |
| `src/Window.cpp` | window discovery, subclassing, synthetic event dispatch, focus shield, placement |
| `src/ApiHooks.cpp` | user32 hooks (cursor, key state, foreground, activation, raw input) |
| `src/GameInputHooks.cpp` | GameInput v2 hooks (virtual mouse, keyboard readings) |
| `src/Audio.cpp` | FMOD hooks: the game's master channel group is muted in background mode (`mute`) |
| `src/NetHooks.cpp` | loopback-only network in background mode (`loopbackOnly`: no firewall prompt, no queries to servers off the PC) |
| `src/Pipe.cpp` | control pipe and protocol |
| `src/Manager.cpp` | link to the session's manager window |
| `src/Trace.cpp` | trace-mode input logging |
| `src/MoveInput.cpp` | `input` op: move override in the client's `ClientInputUpdateSystem` with a dead-man TTL (`gameHooks`) |
| `src/Send.cpp` | `send` op (`allowSend`): allowlisted serverbound packets, sent by the local player on the client thread |
| `src/GameState.cpp`, `src/Look.cpp` | `world` snapshot and absolute `look` (`gameHooks`) |
| `src/GameActions.cpp` | `slot`, `action` and tick-based `button` ops (`gameHooks`) |
| `src/Stack.cpp` | `stack` op (`allowStack`): item stack requests sent by the local client |
| `src/TapSink.h` | the frame type and sink chain every tap publisher shares (keep it byte-identical) |
| `src/Tap.cpp`, `src/Tap.h` | tap pipe: framing, markers, writer thread, the `tap` op |
| `src/NetTap.cpp` | tap capture on a remote server link, and the blob cache switch |
| `src/TapLoopback.cpp` | tap capture in singleplayer and LAN-host worlds (re-serialized packets) |
| `src/TapJournal.cpp` | the tap's join journal (compacted replay for a late reader) |
| `src/MemoryOperators.cpp` | routes `new`/`delete` through LeviLamina's allocator (required by LeviLamina) |
| `client/` | PowerShell client, harness shim, game start (`Start-BeDDGame.ps1`), manager window (`BeDDManager.ps1`), clone script and setup (`BeDDSetup.psm1`, `Initialize-BeDD.ps1`: finds the game instance and sets up what a new PC lacks), all shipped next to the DLL |

When you hook a game or LeviLamina symbol, check the SDK headers under
`%LOCALAPPDATA%\.xmake\packages\l\levilamina\<version>\<hash>\include` first. Only call `MCAPI`,
`MCFOLD` and `MCTAPI` functions.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep each pull request focused on one change. Describe what it does and how you tested it.
3. Follow the existing style: C++20, the provided `.clang-format` and `.editorconfig`, English in code and comments.
4. Test in game: `tools/dev.ps1 -Trace`, then drive the game through `BeDD.psm1` and check that
   the input works, the game never takes the foreground (`tools/out/desktop-watch.log`) and real input
   does not reach it.
5. CI must pass before a pull request is merged.

Commit messages use the imperative mood ("Add wheel tilt", not "Added …"). Update
[CHANGELOG.md](CHANGELOG.md) under *Unreleased* for user-visible changes.

## Development

Material for people working on Be.D.D. itself, or talking to its pipe from something
other than `BeDD.psm1`. Script authors find everything they need in the [README](README.md).

### Building

You need Windows x64, [xmake](https://xmake.io), Visual Studio Build Tools with MSVC 14.4x or newer, and LLVM (`clang-cl`).
`tools/build.ps1` finds them, installs what is missing with winget, and builds:

```powershell
pwsh -NoProfile -File tools/build.ps1   # [-Mode debug]
```

By hand:

```powershell
xmake f -a x64 -m release -p windows --target_type=client -y
xmake -y        # -> bin\BeDD\ (DLL, manifest and everything in client\)
```

The mod packer reads the version from `git describe`, so the repository needs at least one commit.

`tools\dev.ps1 [-Trace]` is a playground: it takes the shared e2e lock
(`%APPDATA%\levilauncher.exe\bei-e2e.lock`), deploys the mod into the instance BeDD finds (set up
first on a new PC, see the README), launches it through LeviLauncher and logs every
foreground and cursor-clip change to `tools\out\desktop-watch.log` until the game exits or
`tools\out\dev.stop` exists (`tools\out\dev.restart` redeploys and relaunches without giving up the
lock). It always closes the game, removes the mod and releases the lock.

### Showcase media

`docs/showcase/` holds the gallery shown on LeviMC Mods (`showcase.json`) and the README pictures.
`tools/showcase/record.ps1 [-KeepOpen]` reproduces the desktop video: under the shared e2e lock
(plus the `hi<N>-e2e.lock` of each clone) it prepares three clone instances (`-hi2`..`-hi4`, other
mods moved aside), restores the `tools/showcase/fixtures/Showcase.zip` world, starts the games in
the background, enters the world, lays the windows out in a 2x2 grid over a backdrop (`stage.ps1`)
with Windows Terminal running `demo.ps1` in the fourth cell, and records that region of the real
screen with ffmpeg's Desktop Duplication grabber to `tools/showcase/out/take-<n>.mkv`. With
`-KeepOpen`, creating `tools/showcase/out/take` starts another take and `tools/showcase/out/stop`
ends the session. `tools/showcase/encode.ps1` cuts a take into the MP4, WebP and poster files.

### How it works

```
 test script (pwsh)                      Minecraft.Windows.exe + BeDD.dll
 ──────────────────                      ─────────────────────────────────────────────
 Send-BeDDKey E ──JSON line──▶ \\.\pipe\BeDD
                                          │ queue Event{WM_KEYDOWN 'E'}; PostMessage(hwnd, WM_LHI)
                                          ▼
                                   subclassed window procedure (game thread)
                                     WM_LHI → apply virtual key/cursor state
                                            → CallWindowProc(game's WndProc, WM_KEYDOWN, ...)
                                     real WM_KEY*/WM_MOUSE*/WM_INPUT/WM_POINTER* → swallowed
                                     WM_ACTIVATEAPP(0)/WM_KILLFOCUS/WM_ACTIVATE(inactive) → swallowed
 ◀──{"ok":true}── after the game thread dispatched it
```

1. **Keyboard, text and pointer position go through the game's own window procedure.** The mod
   subclasses the game window. Keys become `WM_KEYDOWN`/`WM_KEYUP`, text `WM_CHAR`, the pointer
   `WM_MOUSEMOVE` plus a virtual cursor (what the hooked `GetCursorPos` returns), dispatched on the
   game thread in order. The game handles them exactly like real input
   (`MainGameCore::WndProc_PC::onKeyboardAction` → `HIDControllerGameCoreDesktop::onKeyDown`), so
   chat, key mappings and UI hover work. The reply is sent only after the game thread processed
   the input, so scripts are deterministic.
2. **Mouse buttons, wheel and camera go through a virtual GameInput mouse.** Bedrock reads the
   mouse with the GDK **GameInput** API (`GameInputRedist.dll`, interface v2), straight from the
   device: window messages such as `WM_LBUTTONDOWN` are ignored, and without the mod your real
   mouse would still click and turn the camera in a background game. The mod hooks
   `IGameInputReading::GetMouseState` (and `GetKeyCount`/`GetKeyState`): in background mode the game
   reads virtual buttons, relative position (camera), wheel and absolute position instead. GameInput
   has no reading for a mouse until the device reports input, so on an untouched PC the game's
   `GetCurrentReading` gets a virtual reading of that device from the mod.
3. **The game never takes the foreground or loses focus.** Focus-loss messages are swallowed, so
   the game does not pause or release its keys. `Start-BeDDGame.ps1` starts the exe with
   `SW_SHOWNOACTIVATE`; the hooked `ShowWindow`/`SetWindowPos`/`SetActiveWindow`/
   `BringWindowToTop`/`SetForegroundWindow` never activate the game's windows, and the window is
   kept at the bottom of the Z order. In background mode the user32 hooks are installed in `load()`
   already: the game creates and shows its window before `enable()`. The "focus gained" messages
   the mod sends skip the default `WM_ACTIVATE` handling (hooked `DefWindowProcW`), which would set
   the focus and so activate the window: a game started from the user's foreground app (a terminal,
   an agent) is allowed to take the foreground.
4. **Your real input never reaches the game.** Real client-area input messages are swallowed, and
   the user32 calls through which the game reads or grabs the devices see a *virtual* state:
   `ClipCursor` (no-op: your cursor is never confined), `SetCursorPos` (moves the virtual cursor
   only), `GetCursorPos`/`GetPhysicalCursorPos`, `GetAsyncKeyState`, `GetKeyState`,
   `GetKeyboardState`, `GetForegroundWindow`/`GetActiveWindow`/`GetFocus` (report the game window),
   `GetRawInputData`/`GetRawInputBuffer` (real raw input is dropped).
5. **Screenshots** use `PrintWindow(PW_RENDERFULLCONTENT)`, which works while the window is covered.
6. **The manager window** (`client/BeDDManager.ps1`, WinForms in C# compiled by `Add-Type`, one
   per logon session) shows every headless game through a DWM thumbnail, which stays live while
   the game window is off screen. `Start-BeDDGame.ps1` starts it through WMI
   (`Win32_Process.Create` inside `conhost --headless`), so it is not part of the caller's process
   tree or job and survives the agent or harness that happened to start it. The mod links to it in
   `load()`; a linked game creates its window as a tool window off screen (hooked
   `CreateWindowExW`: the game does not pump messages before `enable()`, so the window cannot be
   changed from outside its thread until then) and keeps it there (`SetWindowPos` moves are
   dropped, a watcher puts it back). The manager puts each linked game into a kill-on-close job of
   its own (once linked, so the start-up is left alone; a game that never links joins after a
   minute): closing or killing the manager closes the games.

The environment variable `BEDD_RETURN_FOCUS` (a window handle, set by
`Start-BeDDGame.ps1` and the shim's `Start-Game`) tells the mod which window gets the foreground
back if the game still activates itself while starting.

### Pipe protocol

`\\.\pipe\<pipeName>` (local only, default `BeDD`), UTF-8, one JSON object per line
each way. An `id` is echoed. Replies are `{"ok":true,...}` or `{"ok":false,"error":"..."}`. Input
ops reply after the game thread has dispatched the input (`timeoutMs`, default 5000). The protocol
version (`ping`.`protocol`) stays 1: newer ops are found by calling them (below).

| op | fields | |
|---|---|---|
| `ping` | | `protocol`, `pid`, `hwnd`, `background`, `client{w,h}`, `cursor{x,y}`, `realForeground`, `managed`, `takenOver` |
| `state` | | `ping` fields + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput, loopbackRebinds, serverQueriesRefused, audio, tapLoopback, send, journal}`, `clients` (control-pipe clients connected now), `hooks{name: installed}` (each game hook a feature installed; `false` while the feature is off) |
| `key` | `key` (name) or `vk`; `action` press\|down\|up; `holdMs` | names: letters, digits, ESC, ENTER, BACK, TAB, SPACE, SHIFT, CTRL, ALT, LEFT/UP/RIGHT/DOWN, HOME, END, PGUP, PGDN, INSERT, DELETE, F1..F24, SLASH, GRAVE, MINUS, EQUALS |
| `text` | `text`; `delayMs` | one `WM_CHAR` per UTF-16 unit |
| `mouse` | `action` move\|down\|up\|click\|wheel; `x`,`y` (client px) or `rx`,`ry` (fractions); `button` left\|right\|middle; `notches`; `holdMs` | moves the virtual cursor (if a point is given), then buttons/wheel on the virtual GameInput mouse |
| `raw` | `action` move\|down\|up\|click\|wheel; `dx`,`dy` (mouse counts); `steps`; `button`; `notches`; `holdMs` | virtual GameInput mouse: camera while the mouse is grabbed |
| `release` | | releases every held key and button, whoever pressed it, and ends the `input` move override |
| `focus` | | re-sends the "focus gained" messages |
| `window` | `clientWidth`, `clientHeight`, `placement` back\|offscreen\|none | never activates the window |
| `background` | `on` | background mode on/off |
| `flush` | | waits until everything posted was dispatched |

#### Several clients

Several clients can be connected at once (config `pipeInstances`, default 4; 1 = one at a time),
each served by its own thread. Ops that post window input are serialized, so two clients never
interleave a press or a typed text; `ping`, `state`, `tap`, `world`, `look`, `input`, `send` and
`stack` run without that lock (they only read, or hand their work to the client thread). What a
client holds is released when it disconnects (or its process dies), and nothing another client
holds: its keys and mouse buttons, the `button` holds it made and the `input` override it set. Ops
the manager window forwards over the manager link are neither serialized nor owned.

For units with per-client state (`src/BeDD.h`): `currentPipeClient()` is the id of the client the
current thread serves (from 1, never reused; 0 on the manager link and the game thread). Tag the
state with it and release it from `clientGone()` in `src/Pipe.cpp` (as `clearMoveInputOf` and
`releaseGameButtonsOf` do), or register `onPipeClientGone(fn)` in `enable()`: either runs on the
departing client's thread, after its keys and buttons are released, with input ops serialized
(not at shutdown).

#### Ops for bedboo (transport `bedd`)

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
| `look` | `gameHooks` | `{inLevel, yaw, pitch, tick}` | `yaw`, `pitch`, `timeoutMs` (1000, at most 60000) |
| `input` | `gameHooks` | `active`, `via` hooks\|keys, `hooks{raw,state}`, `tick`, `applied` | `move{x,z}`, `jump`, `sneak`, `sprint`, `ttlMs` (300) |
| `slot` | `gameHooks` | `{slot}` (null outside a world) | `slot` 0..8 |
| `action` | `gameHooks` | `{names}` | `name` drop\|dropAll\|inventory\|perspective\|pause |
| `button` | `gameHooks` | `{held}` | `button` attack\|use\|pick; `action` down\|up\|click; `holdTicks`; `releaseOn` blockChange |
| `send` | `allowSend` | `{allow}` (the ids in `sendAllow`) | `buf`, `timeoutMs` (2000, at most 10000) |
| `stack` | `allowStack` | `{enabled, via:"raw", stats{sent, answered, rejected, failed, resyncs}}` | `buf`, `timeoutMs` (2000); or `view:true` |

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
  once.
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
  `not_in_level`, `busy` (a screen is open; `action pause` and `button up` still work), `timeout`,
  `bad_args`, `refused`. A paused singleplayer world does not tick: after `action pause` there,
  resume with the `key` op (ESC).
  - `slot`: `ClientInputCallbacks::handleSlotSelectButtonPress`, and when that leaves the slot
    alone (it does for a keyboard player in 1.26.51) the hotbar key `1`..`9` of the default key map.
    The client sends `mob_equipment` itself. Replies `slot` and `via` callback\|keys.
  - `action`: the matching `ClientInputCallbacks::handle*ButtonPress` (inventory with
    `CraftingType::Inventory`).
  - `button`: left/right/middle on the virtual GameInput mouse (needs background mode), released
    by the tick listener after `holdTicks` (click: default 2) or, with `releaseOn:"blockChange"`,
    once the block the client's hit result pointed at when it went down changes, turns to air or is
    no longer targeted (`refused` when no block is targeted). `up` applies at once. Never blocks
    the pipe. Replies `held`, and `block{x,y,z}`.
- **`send`**: `buf` is the base64 of one serverbound packet (varint header + body, as
  bedrock-protocol serializes it). It is rebuilt on the client thread
  (`MinecraftPackets::createPacket` + `readNoHeader` with the client NetworkSystem's
  `ReflectionCtx`) and sent through the local player (`sendNetworkPacket`), on a remote server and
  in singleplayer alike; the reply `{packetId, name}` comes once it is sent. Bytes the game does not
  read are `bad_args` and nothing is sent; an id not in `sendAllow` is `refused`. `state` reports
  `stats.send{sent, refused, failed, lastError}`.
- **`stack`**: see below.

**Security.** `send` lets any process of the current user that can open the pipe make the client
send packets as the signed-in player, so it is off by default (`allowSend`) and limited to
`sendAllow` (default text 9 and command_request 77: chat and commands, as the player could type
them). Ids 1 (login), 30 (inventory_transaction), 144 (player_auth_input) and 147
(item_stack_request) are always refused, even when listed: the client owns them, and forged ones
would fight its prediction and bookkeeping. Widen `sendAllow` only for a server you control. While
`allowStack` is on, anything that can open the pipe can move, craft, drop and destroy items of the
signed-in player on whatever server the client is on. The tap carries the whole session of the
signed-in player. Keep all three off unless a harness needs them; the manager pipe never turns
them on.

#### `stack`: item stack requests through the client (`allowStack`)

| Form | Reply |
|---|---|
| `{op:"stack"}` (probe) | `{ok, enabled:true, via:"raw", stats:{sent, answered, rejected, failed, resyncs}}` |
| `{op:"stack", buf, timeoutMs=2000}` | `{ok, via:"raw", requestIds:[client ids], map:{"<caller id>": client id}}` |
| `{op:"stack", view:true}` | `{ok, inventory:[...], ui:[...], open?:{containerId, type, items:[...]}}`: what the client shows, each `{slot, item, count, stackId}` |

- `buf` is the base64 of one whole item_stack_request packet (id 147), varint header plus body, as
  bedrock-protocol serializes it. Any other packet id, and bytes the game does not read, are `bad_args`.
- On the next client tick the packet is rebuilt with `MinecraftPackets::createPacket` and
  `readNoHeader`, and the local player sends it.
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

#### Who sends what (bedboo transport `bedd`)

The real client owns every serverbound packet; bedboo turns what it would have sent into intents.
This table is the same as bedboo's `lib/bedd/authority.js`.

| Class | Packets | Handling |
|---|---|---|
| clientOwned | login, client_to_server_handshake, request_network_settings, resource_pack_client_response, client_cache_status, client_cache_blob_status, request_chunk_radius, subchunk_request, serverbound_loading_screen, set_local_player_as_initialized, network_stack_latency, tick_sync, player_skin, emote_list, respawn, player_action (respawn / dimension_change_ack / start_item_use_on / stop_item_use_on), animate, interact mouse_over_entity | bedboo drops them (counted): the client sends its own. A respawn is the user clicking Respawn on the client |
| translated | player_auth_input (move_vector + flags to `input`; yaw/pitch to `look`; block_actions start_break/abort_break/stop_break to `button attack`), mob_equipment (`slot`), inventory_transaction (item_use click_block/click_air, item_release, item_use_on_entity: `look`, then `button use`/`attack`), interact open_inventory (`action inventory`), container_close (key ESC), item_stack_request (`stack`) | bedboo's router turns them into ops |
| shared | text, command_request | sent through `send`; fallback: T or SLASH, then `text`, then ENTER |
| unsupported | book_edit, block_pick_request, crafting_event, set_default_game_type, everything else | bedboo throws BeddUnsupportedError (ERR_BEDD_UNSUPPORTED) |

item_stack_request is unsupported only when the `stack` probe does not answer `ok`.

### Tap protocol

`\\.\pipe\<pipeName>-tap`, only with config `tap` (off by default). One reader at a time, local
and current user only. BeDD writes, the reader only reads. It carries every clientbound game
packet the client receives, an echo of every serverbound packet it sends (not login, the
handshakes or request_network_settings), and JSON markers. bedboo derives the name as
`<control pipe>-tap`.

Frames, little-endian:

```
u32 len (bytes after this field) | u8 kind | u8 flags | u16 0 | u64 seq | u64 tUs (steady clock µs) | payload
```

`len` is at most 16 MiB + 20. `seq` rises over the process (replayed frames keep theirs).

| kind | | payload |
|---|---|---|
| 1 | CLIENTBOUND | one game packet: varint header (`id \| senderSub<<10 \| targetSub<<12`) + body, as bedrock-protocol's `parsePacketBuffer` reads it |
| 2 | SERVERBOUND_ECHO | the same, for a packet the client sent (never ids 1, 3, 4, 193) |
| 4 | MARKER | UTF-8 JSON |

Readers ignore other kinds. Flags: 1 replay (from the join journal), 2 reserialized
(`Packet::write`, not wire bytes), 4 local link, 8 suppressed (reserved).

| marker | fields |
|---|---|
| `HELLO` (always first) | `tap:1`, `mod`, `game:"1.26.51.01"`, `protocol` (2193), `pid`, `link{state, type, address}`, `journal{frames, bytes, complete}` (what this reader is replayed) |
| `LINK_UP` | `type` remote\|local, `address` |
| `LINK_DOWN` | `reason` |
| `REPLAY_BEGIN` | `frames` |
| `REPLAY_END` | `complete` (false when the journal went past `tapJournalMiB`) |
| `DROPPED` | `count`; the pipe closes after it |

A reader gets HELLO, REPLAY_BEGIN, the join journal's frames (flag replay), REPLAY_END, then the
live stream; live frames that arrive during the replay wait until REPLAY_END, and live frames the
replay already held are skipped by seq. Without a journal the pair is empty (`complete:false`). A
reader more than 32 MiB behind gets DROPPED and is disconnected; reconnecting gives a full replay.

All publishers share `src/TapSink.h`, which must stay byte-identical: each installs its sink with
`prev = gEmit.exchange(&own)`, forwards every frame to `prev`, and on removal puts `prev` back
only if `gEmit` is still its own (compare-exchange). The hooks only copy bytes; the pipe's writer
thread drains its queue.

#### Remote servers (`NetTap.cpp`)

The client's NetworkSystem follows its one outgoing connection (`$onNewOutgoingConnection`;
`$onConnectionClosed`, `$onAllConnectionsClosed`, `closeConnection` and `disconnect` end it). On
a remote link, `NetworkConnection::receivePacket` (after it, `HasData`) gives each decompressed,
decrypted packet, and `NetworkSystem::_sendInternal` each serialized packet the client sends: wire
bytes, flags 0. With config `trace`, the first 50 packets of each link are logged as
`tap trace #n in|out id len`: they come in the order of a BDS capture (network_settings,
server_to_client_handshake, play_status, resource_packs_info, ..., start_game, ...).

**Blob cache.** A client with its blob cache on gets sub-chunks as blob hashes and only the blobs
it misses (`client_cache_miss_response`); the rest come from its cache on disk and never cross the
wire, so a reader could not build the world. While the tap is on, BeDD therefore clears `enabled`
in the client's `client_cache_status` before it is sent, and servers send every sub-chunk in full.
The echo shows `enabled:false`.

#### Singleplayer and LAN-host worlds (`TapLoopback.cpp`, config `tapLoopback`)

`tapLoopback` (default `true`, effective only with `tap`) puts singleplayer worlds and LAN worlds
the client hosts on the tap pipe. The client then talks to its in-process server through the
LocalConnector and the remote capture publishes nothing; `LoopbackPacketSender` is hooked instead,
and each packet is re-serialized (varint header `id | senderSub << 10 | targetSub << 12`, then
`Packet::write` with the sending `NetworkSystem`'s `ReflectionCtx`). Frames carry the flags
`reserialized` (2) and `local` (4).

- **CLIENTBOUND**: the server's sender (`$sendToClient` both overloads, `$sendToClients`,
  `$sendBroadcast` both overloads, `$sendTo`), only for a `NetworkIdentifier` whose connection on
  the server is of type `Local`. LAN guests are remote connections and are never published
  (counted as `guest`). Broadcasts are published once the local player got `start_game`.
- **SERVERBOUND_ECHO**: the client's sender (`$sendToServer`, `$send`, `$sendTo`), never ids 1, 3, 4
  or 193.
- Chunks: the integrated server answers every `subchunk_request` through the same sender, so each
  `subchunk` and `subchunk_request` the client exchanges is on the tap (checked in game: as many
  `subchunk` frames as `ServerNetworkHandler::handle(SubChunkRequestPacket)` calls). The client
  asks relative to one origin (its own sub-chunk) and only up to each column's
  `highest_subchunk_count`; a local client sends `client_cache_status` with `enabled:false`, so
  every entry carries its payload.
- Only while the client's link (`tap::gLinkType`) is local. `$onNewOutgoingConnection` /
  `$onConnectionClosed` on the client `NetworkSystem` set it from the connection type, as the tap
  core does, so the loopback tap works without it. While no link is known yet (the in-process
  server already sends before the client's connect call returns), a `Local` connection of the
  client counts; that check never writes `gLinkType`. The remote capture only publishes on a
  `Remote` link (`_sendInternal` also runs for the local link), so no packet is published twice. A
  sender method that calls another publishes a packet once.
- Without a tap pipe (`tap::gEmit` null) frames are only counted. `state` reports
  `stats.tapLoopback{enabled, hooks, link, clientbound, echo, bytes, unpublished, guest}`.
- With `trace`, the first 50 frames are logged (`trace tap loopback: in|out id= len=`) and the first
  4000 are written to `mods\BeDD\data\tap-loopback.bin` in the tap pipe's framing (seq and tUs 0).

#### Join journal (`TapJournal.cpp`)

With `tap` on, the journal chains onto `bedd::tap::gEmit` and sets `bedd::tap::gReplay`, which the
tap pipe calls for each new reader between `REPLAY_BEGIN` and `REPLAY_END`. Replayed frames keep
their seq and order and carry flag `replay`.

- Only CLIENTBOUND frames are kept. The journal resets on a new link (the `LINK_UP` marker or a
  change of `gLinkType`) and on `start_game` (11), which becomes its first frame.
- Compaction keys (read from the leading fields only; the last frame with a key wins):
  `level_chunk` (x, z, dimension), `update_block` (position,
  layer), `set_time`, `set_health`, `network_chunk_publisher_update`, `current_structure_feature`,
  `inventory_content` (window), `inventory_slot` (window, slot), `level_event` (event id, so the
  weather keeps its last state), `update_abilities` (unique id), the `add_*` packets (unique id;
  `add_player` by runtime id), and per runtime id `set_entity_motion`, `move_entity`,
  `move_player` and `mob_armor_equipment`. Packets that carry only the fields that changed are
  keyed by entity and the set of fields, so every field keeps its last value: `set_entity_data`
  (the metadata keys), `update_attributes` (the attribute names), `move_entity_delta` (the fields
  present) and `mob_effect` (the effect).
- `subchunk` is kept per sub-chunk, not per packet: the client asks for all its sub-chunks relative
  to one origin, so a packet is dropped only once newer frames carry every sub-chunk in it (origin +
  each entry's offset). A packet the journal cannot walk is kept whole.
- `remove_entity` (14) deletes the entity's `add_entity`/`add_item_entity`/`add_player` frame
  (`add_player` is matched by its unique id's bytes) and everything keyed by its runtime id.
- Not kept: `text`, `animate`, `network_stack_latency`, `play_sound`, `stop_sound`,
  `level_sound_event` (and v2), `spawn_particle_effect`, `level_event_generic`.
- Past `tapJournalMiB` (default 128) frames are no longer kept and the journal reports
  `complete:false` until the next reset: a late reader then misses chunks until the player
  re-enters them or the client rejoins.
- `state` reports `stats.journal`: `enabled`, `frames`, `bytes`, `complete`, and the counters
  `seen` (clientbound frames), `compacted` (replaced by a later frame with the same key), `pruned`
  (entities whose add frame `remove_entity` deleted), `transient` (not kept), `overCap` (not kept
  past `tapJournalMiB`), `resets` and `replays`.

### Manager pipe

`\\.\pipe\BeDD-Manager-<session id>` (current user only), served by
`BeDDManager.ps1`, JSON lines like the control pipe.

- **Clients** (one request per connection): `ping` (`games`; `ok` is false while it closes),
  `adopt` `{pid, instance, label}` (`Start-BeDDGame.ps1` right after starting a game), `list`
  (`games[]`: `pid`, `instance`, `label`, `pipe`, `hwnd`, `linked`, `background`, `managed`,
  `takenOver`, `job`, `thumbnail`), `game` `{pid, action: show|hide|close}`, `shutdown`.
- **Games** (the mod): the first line is `hello` `{protocol, pid, hwnd, pipe, instance, label,
  background}` (`label` from `BEDD_LABEL`), answered with `{"ok":true}` (a manager that
  is closing hangs up instead; the game is linked only once welcomed). The connection stays open
  and the manager sends requests on it: every control-pipe op (it polls `state` every second), plus `show` (take over:
  background off, a normal window on screen and in front; the manager calls
  `AllowSetForegroundWindow` first), `hide` (hand back) and `close` (`WM_CLOSE` to the game window).

## License

By contributing you agree that your contributions are licensed under the [LGPL-3.0-or-later](COPYING.LESSER), the license of this project.
