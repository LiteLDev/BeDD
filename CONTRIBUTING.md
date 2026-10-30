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
ops reply after the game thread has dispatched the input (`timeoutMs`, default 5000).

| op | fields | |
|---|---|---|
| `ping` | | `protocol`, `pid`, `hwnd`, `background`, `client{w,h}`, `cursor{x,y}`, `realForeground` |
| `state` | | `ping` fields + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput}`, `clients` (control-pipe clients connected now) |
| `key` | `key` (name) or `vk`; `action` press\|down\|up; `holdMs` | names: letters, digits, ESC, ENTER, BACK, TAB, SPACE, SHIFT, CTRL, ALT, LEFT/UP/RIGHT/DOWN, HOME, END, PGUP, PGDN, INSERT, DELETE, F1..F24, SLASH, GRAVE, MINUS, EQUALS |
| `text` | `text`; `delayMs` | one `WM_CHAR` per UTF-16 unit |
| `mouse` | `action` move\|down\|up\|click\|wheel; `x`,`y` (client px) or `rx`,`ry` (fractions); `button` left\|right\|middle; `notches`; `holdMs` | moves the virtual cursor (if a point is given), then buttons/wheel on the virtual GameInput mouse |
| `raw` | `action` move\|down\|up\|click\|wheel; `dx`,`dy` (mouse counts); `steps`; `button`; `notches`; `holdMs` | virtual GameInput mouse: camera while the mouse is grabbed |
| `release` | | releases every held key/button |
| `focus` | | re-sends the "focus gained" messages |
| `window` | `clientWidth`, `clientHeight`, `placement` back\|offscreen\|none | never activates the window |
| `background` | `on` | background mode on/off |
| `flush` | | waits until everything posted was dispatched |
| `world` | | config `gameHooks`. `inLevel`, `levelEpoch` (+1 per world joined), `topScreen` (e.g. `hud_screen`, `pause_screen`), `mouseGrabbed`, `showingMenu`, `inputMode` mouse\|touch\|gamepad\|undefined, `player{runtimeId, eye{x,y,z}, rot{yaw,pitch}, slot}` (null outside a world), `tick` (client ticks seen). A snapshot the client thread takes: the screen fields every frame, the player every client tick |
| `look` | `yaw`, `pitch` (degrees, as in player_auth_input: yaw 0 = +Z, 90 = -X; pitch -90 = up, clamped to +-89.9 like the game does); `timeoutMs` (default 1000, at most 60000) | config `gameHooks`. Turns the player with `LocalPlayer::localPlayerTurn` on client ticks and replies `yaw`, `pitch`, `tick`, `turns` once both are within 0.5 degrees. Without `yaw`/`pitch`: `{inLevel, yaw, pitch, tick}`. Errors carry `code`: `not_in_level`, `timeout` (with the current `yaw`/`pitch`), `busy` (a newer `look` replaced it), `bad_args` |

`ping` and `state` also report `managed` (linked to the manager window) and `takenOver`.

Ops behind a config key reply `{"ok":false,"code":"disabled","key":"<key>"}` while it is off.
`state` also has `hooks` `{name: installed}`: each game hook a feature installed (all `false`
while the feature is off).

Errors of newer ops may carry `code`: `disabled` (the feature is off; `error` names the
config.json key), `not_in_level`, `refused`, `timeout`, `bad_args`, `busy`. An op this build lacks
answers `unknown op 'x'`.

| op | fields | |
|---|---|---|
| `tap` | | read-only; `code:"disabled"` without config `tap`. `enabled`, `pipe`, `attached` (a reader is connected), `link{state up\|down, type remote\|local, address}`, `journal{frames, bytes, complete}` (what the last reader was replayed), `replay` (a join journal is installed), `counters{in, out, dropped, sent, readers}`, `hooks` |

### Tap protocol

`\\.\pipe\<pipeName>-tap`, only with config `tap` (off by default). One reader at a time, local
and current user only. BeDD writes, the reader only reads. It carries every clientbound game
packet the client receives, an echo of every serverbound packet it sends (not login, the
handshakes or request_network_settings), and JSON markers.

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
| `HELLO` (always first) | `tap:1`, `mod`, `game:"1.26.51.01"`, `protocol` (2193), `pid`, `link{state, type, address}`, `journal{frames, bytes, complete}` |
| `LINK_UP` | `type` remote\|local, `address` |
| `LINK_DOWN` | `reason` |
| `REPLAY_BEGIN` | `frames` |
| `REPLAY_END` | `complete` |
| `DROPPED` | `count`; the pipe closes after it |

A reader gets HELLO, REPLAY_BEGIN, the join journal's frames (flag replay), REPLAY_END, then the
live stream; live frames that arrive during the replay wait until REPLAY_END. Without a journal
the pair is empty (`complete:false`). A reader more than 32 MiB behind gets DROPPED and is
disconnected; reconnecting gives a full replay.

Sources (`NetTap.cpp`): the client's NetworkSystem follows its one outgoing connection
(`$onNewOutgoingConnection`; `$onConnectionClosed`, `$onAllConnectionsClosed`,
`closeConnection` and `disconnect` end it). On a remote link, `NetworkConnection::receivePacket` (after it,
`HasData`) gives each decompressed, decrypted packet, and `NetworkSystem::_sendInternal` each
serialized packet the client sends. The hooks only copy bytes into a queue; one writer thread
drains it. Other tap publishers (the join journal, the loopback tap) share `src/TapSink.h`, which
must stay byte-identical in every unit: each installs its sink with
`prev = gEmit.exchange(&own)`, forwards every frame to `prev`, and on removal puts `prev` back
only if `gEmit` is still its own (compare-exchange).

With config `trace`, the first 50 packets of each link are logged as `tap trace #n in|out id len`:
they come in the order of a BDS capture (network_settings, server_to_client_handshake,
play_status, resource_packs_info, ..., start_game, ...).

#### Who sends what (bedboo transport `bedd`)

The real client owns every serverbound packet; bedboo turns what it would have sent into intents.
This table is the same as bedboo's `lib/bedd/authority.js`.

| Class | Packets | Handling |
|---|---|---|
| clientOwned | login, client_to_server_handshake, request_network_settings, resource_pack_client_response, client_cache_status, client_cache_blob_status, request_chunk_radius, subchunk_request, serverbound_loading_screen, set_local_player_as_initialized, network_stack_latency, tick_sync, player_skin, emote_list, respawn, player_action (dimension_change_ack / start_item_use_on / stop_item_use_on), animate, interact mouse_over_entity | bedboo drops them (counted) |
| translated | player_auth_input (move_vector + flags to `input`; yaw/pitch to `look`; block_actions start_break/abort_break/stop_break to `button attack`), mob_equipment, inventory_transaction (item_use click_block/click_air, item_release, item_use_on_entity), interact open_inventory, container_close, item_stack_request (through `stack`) | bedboo's router turns them into ops |
| shared | text, command_request | sent through `send`; fallback: T or SLASH, then `text`, then ENTER |
| unsupported | book_edit, block_pick_request, crafting_event, set_default_game_type, everything else | bedboo throws BeddUnsupportedError (ERR_BEDD_UNSUPPORTED) |
Several clients can be connected at once (config `pipeInstances`, default 4; 1 = one at a time),
each served by its own thread. Ops that post input (everything but `ping`, `state`, `tap` and
`world`) are serialized, so two clients never interleave a press or a typed text. Each key and
mouse button remembers the client that pressed it last; when a client disconnects (or its process
dies), BeDD releases what it still holds, and nothing another client holds. The `release` op still
releases every held key and button, whoever pressed it. Ops the manager window forwards over the
manager link are neither serialized nor owned (as before).

For units with per-client state (`src/BeDD.h`): `currentPipeClient()` is the id of the client the
current thread serves (from 1, never reused; 0 on the manager link and the game thread), and
`onPipeClientGone(fn)` calls `fn(id)` on the departing client's thread, after its keys and
buttons are released and with input ops still serialized (not at shutdown). Register in
`enable()`, or add the one-line release call to `clientGone()` in `src/Pipe.cpp`.
`state` also reports `stats.journal` (the tap's join journal, `src/TapJournal.cpp`, config `tap`):
`enabled`, `frames`, `bytes`, `complete` (what a tap client attaching now gets replayed), and the
counters `seen` (clientbound frames), `compacted` (replaced by a later frame with the same key),
`pruned` (entities whose add frame `remove_entity` deleted), `transient` (not kept), `overCap`
(not kept past `tapJournalMiB`), `resets` and `replays`.

#### Tap join journal

With `tap` on, the journal chains onto `bedd::tap::gEmit` (`src/TapSink.h`: it keeps the sink that
was there and forwards every frame to it, so it works with or without the tap pipe and in either
install order) and sets `bedd::tap::gReplay`, which the tap pipe calls for each new client between
`REPLAY_BEGIN` and `REPLAY_END`. Replayed frames keep their seq and order and carry flag `replay`.

- Only CLIENTBOUND frames are kept. The journal resets on a new link (the `LINK_UP` marker or a
  change of `gLinkType`) and on `start_game` (11), which becomes its first frame.
- Compaction keys (read from the leading fields only; the last frame with a key wins):
  `level_chunk` (x, z, dimension), `subchunk` (dimension, origin), `update_block` (position,
  layer), `set_time`, `set_health`, `network_chunk_publisher_update`, `current_structure_feature`, `inventory_content` (window),
  `inventory_slot` (window, slot), `level_event` (event id, so the weather keeps its last state),
  `update_abilities` (unique id), the `add_*` packets (unique id; `add_player` by runtime id), and
  per runtime id `update_attributes`, `set_entity_data`, `set_entity_motion`, `move_entity`,
  `move_entity_delta`, `move_player` and `mob_armor_equipment`.
- `remove_entity` (14) deletes the entity's `add_entity`/`add_item_entity`/`add_player` frame
  (`add_player` is matched by its unique id's bytes) and everything keyed by its runtime id.
- Not kept: `text`, `animate`, `network_stack_latency`, `play_sound`, `stop_sound`,
  `level_sound_event` (and v2), `spawn_particle_effect`, `level_event_generic`.
- Past `tapJournalMiB` (default 128) frames are no longer kept and the journal reports
  `complete:false` until the next reset: a late tap client then misses chunks until the player
  re-enters them or the client rejoins.

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

### Loopback tap

`tapLoopback` (default `true`, effective only with `tap`; `TapLoopback.cpp`) puts singleplayer worlds
and LAN worlds the client hosts on the tap pipe. The client then talks to its in-process server
through the LocalConnector, so the remote capture sees nothing; `LoopbackPacketSender` is hooked
instead, and each packet is re-serialized (varint header `id | senderSub << 10 | targetSub << 12`,
then `Packet::write` with the sending `NetworkSystem`'s `ReflectionCtx`). Frames carry the flags
`reserialized` (2) and `local` (4).

- **CLIENTBOUND**: the server's sender (`$sendToClient` both overloads, `$sendToClients`,
  `$sendBroadcast` both overloads, `$sendTo`), only for a `NetworkIdentifier` whose connection on
  the server is of type `Local`. LAN guests are remote connections and are never published
  (counted as `guest`). Broadcasts are published once the local player got `start_game`.
- **SERVERBOUND_ECHO**: the client's sender (`$sendToServer`, `$send`, `$sendTo`), never ids 1, 3, 4
  or 193.
- Only while the client's link (`tap::gLinkType`) is local. `$onNewOutgoingConnection` /
  `$onConnectionClosed` on the client `NetworkSystem` set it from the connection type, as the tap
  core does, so the loopback tap works without it. While no link is known yet (the in-process
  server already sends before the client's connect call returns), a `Local` connection of the
  client counts; that check never writes `gLinkType`. The remote capture only publishes on a
  `Remote` link, so no packet is published twice. A sender method that calls another publishes a
  packet once.
- Without a tap pipe (`tap::gEmit` null) frames are only counted. `state` reports
  `stats.tapLoopback{enabled, hooks, link, clientbound, echo, bytes, unpublished, guest}`.
- With `trace`, the first 50 frames are logged (`trace tap loopback: in|out id= len=`) and the first
  4000 are written to `mods\BeDD\data\tap-loopback.bin` in the tap pipe's framing (seq and tUs 0).

## License

By contributing you agree that your contributions are licensed under the [LGPL-3.0-or-later](COPYING.LESSER), the license of this project.
