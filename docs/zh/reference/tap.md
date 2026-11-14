# Tap 协议

为 [bedboo](https://github.com/LiteLDev/bedboo)（传输 `bedd`）提供的客户端游戏数据包只读流。配置 `tap`
（[配置](/zh/guide/configuration#功能开关)）；状态：[`tap` 操作](/zh/reference/websocket#bedboo-操作)。

## 订阅与帧

`{"op":"tap","action":"subscribe"}` → `{"ok":true}`，之后每帧一条二进制消息。该连接上不要再发其他内容。第二个订阅者：
`busy`；关闭时：`disabled`。

| 字节（LE） | 字段 |
|---|---|
| u32 | `len`：其后的字节数，≤ 16 MiB + 20 |
| u8 | `kind` |
| u8 | `flags`：1 重放，2 重新序列化（`Packet::write`），4 本地链路，8 已抑制（保留） |
| u16 | 0 |
| u64 | `seq`（进程级；重放时保持不变） |
| u64 | `tUs`（单调时钟 µs） |
| … | 负载 |

| kind | | 负载 |
|---|---|---|
| 1 | CLIENTBOUND | varint 头（`id \| senderSub<<10 \| targetSub<<12`）+ 包体（bedrock-protocol `parsePacketBuffer`） |
| 2 | SERVERBOUND_ECHO | 同上；从不含 id 1、3、4、193（login、握手、request_network_settings） |
| 4 | MARKER | UTF-8 JSON（忽略其他 kind） |

| 标记 | 字段 |
|---|---|
| `HELLO`（首个） | `tap:1`、`mod`、`game:"1.26.60.05"`、`protocol`（2225）、`pid`、`link{state, type, address}`、`journal{frames, bytes, complete}` |
| `LINK_UP` | `type` remote\|local、`address` |
| `LINK_DOWN` | `reason` |
| `REPLAY_BEGIN` | `frames` |
| `REPLAY_END` | `complete`（超过 `tapJournalMiB` 时为 false；无日志：空的一对，false） |
| `DROPPED` | `count`；订阅者落后 > 32 MiB；随后断开（重新订阅：完整重放） |

实时帧等到 REPLAY_END 之后；按 seq 跳过重复帧。

发布者共用 `src/TapSink.h`（[代码规范](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md#code-rules)）；钩子只复制，由订阅者线程取走。

## 远程服务器（`NetTap.cpp`）

| | |
|---|---|
| 链路建立 | `NetworkSystem::send`（首个数据包之前）和 `runEvents`（每 tick）找到打开的连接 |
| 链路断开 | `$onConnectionClosed`、`closeConnection`（1.26.60 未导出 `onNewOutgoingConnection`、`onAllConnectionsClosed`、`disconnect`） |
| 入 | `BatchedNetworkPeer::_receivePacket`（其后，`HasData`）：已解密、已解压；flags 0 |
| 出 | `NetworkSystem::_sendInternal`；flags 0 |
| `trace` | 每条链路前 50 个数据包：`tap trace #n in\|out id len` |
| Blob 缓存 | 发送前清除 `client_cache_status.enabled`，使服务器发送完整子区块（回显显示 `false`） |

## 单人世界和局域网主机世界（`TapLoopback.cpp`，配置 `tapLoopback`）

默认 `true`，需要 `tap`。钩住 `LoopbackPacketSender`；数据包重新序列化（头同上，`Packet::write` 使用发送方 `NetworkSystem`
的 `ReflectionCtx`）；flags 2 + 4。

| | |
|---|---|
| CLIENTBOUND | 服务端发送方 `$sendToClient`（两个）、`$sendToClients`、`$sendBroadcast`（两个）、`$sendTo`（自 1.26.60 起经虚表调用 `$sendTo`、`$sendBroadcast(except)`）；仅 `Local` 连接。局域网访客：计入 `guest`。广播在本地玩家的 `start_game` 之后 |
| SERVERBOUND_ECHO | 客户端发送方 `$sendToServer`、`$send`、`$sendTo`；从不含 id 1、3、4、193 |
| 区块 | 每个 `subchunk_request`/`subchunk`（一个原点，最多 `highest_subchunk_count`；`client_cache_status.enabled:false`） |
| 链路 | 仅当 `tap::gLinkType` 为 local；由客户端发送方的首个数据包和 `$onConnectionClosed` 设置。链路未知前，`Local` 客户端连接计入（从不写 `gLinkType`）。远程捕获只在 `Remote` 时发布：不会重复 |
| 无接收端 | `tap::gEmit` 为 null：只计数。`state.stats.tapLoopback{enabled, hooks, link, clientbound, echo, bytes, unpublished, guest}` |
| `trace` | 记录前 50 帧（`trace tap loopback: in\|out id= len=`）；前 4000 帧写入 `mods\BeDD\data\tap-loopback.bin`（seq、tUs 为 0） |

## 加入日志（`TapJournal.cpp`）

挂在 `bedd::tap::gEmit` 链上，设置 `bedd::tap::gReplay`（在 REPLAY 标记之间对每个订阅者调用）。

| | |
|---|---|
| 保留 | 仅 CLIENTBOUND。新链路和 `start_game`（11，首帧）时重置 |
| 每键保留最新 | `level_chunk`（x、z、dim）、`update_block`（pos、layer）、`set_time`、`set_health`、`network_chunk_publisher_update`、`current_structure_feature`、`inventory_content`（window）、`inventory_slot`（window、slot）、`level_event`（事件 id）、`update_abilities`（unique id）、`add_*`（unique id；`add_player` 用 runtime id）；按 runtime id：`set_entity_motion`、`move_entity`、`move_player`、`mob_armor_equipment` |
| 按实体 + 字段 | `set_entity_data`（键）、`update_attributes`（名字）、`move_entity_delta`（字段）、`mob_effect`（效果） |
| `subchunk` | 更新的帧覆盖其全部子区块后丢弃；无法遍历的：保留 |
| `remove_entity`（14） | 删除 add 帧（`add_player` 按 unique id 字节）和 runtime id 键 |
| 不保留 | `text`、`animate`、`network_stack_latency`、`play_sound`、`stop_sound`、`level_sound_event`（v1、v2）、`spawn_particle_effect`、`level_event_generic` |
| 上限 | `tapJournalMiB`（128）：超出后 `complete:false` 直到重置（晚到的订阅者缺区块） |
| 统计 | `state.stats.journal`：`enabled, frames, bytes, complete, seen, compacted, pruned, transient, overCap, resets, replays` |
