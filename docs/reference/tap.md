# Tap protocol

The tap is a read-only binary stream of the game packets the client receives and sends, for
[bedboo](https://github.com/LiteLDev/bedboo)'s transport `bedd`. It is off unless the config turns `tap` on
([Configuration](/guide/configuration#keys-for-bedboo)).

## Pipe and frames

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

## Remote servers (`NetTap.cpp`)

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

## Singleplayer and LAN-host worlds (`TapLoopback.cpp`, config `tapLoopback`)

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

## Join journal (`TapJournal.cpp`)

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

