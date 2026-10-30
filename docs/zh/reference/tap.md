# Tap 协议

tap 是一个只读的二进制流，包含客户端收发的游戏数据包，供 [bedboo](https://github.com/LiteLDev/bedboo) 的 `bedd` 传输使用。
配置打开 `tap` 之前它是关闭的（[配置](/zh/guide/configuration#给-bedboo-用的配置项)）。

## 管道与帧

`\\.\pipe\<pipeName>-tap`，只在配置 `tap` 打开时存在（默认关闭）。一次一个读取方，仅本机、仅当前用户。BeDD 写，读取方只读。
它包含客户端收到的每个客户端方向游戏数据包、它发出的每个服务端方向数据包的回显（不含 login、握手和 request_network_settings），以及 JSON 标记。
bedboo 把名字推导为 `<控制管道>-tap`。

帧，小端序：

```
u32 len (bytes after this field) | u8 kind | u8 flags | u16 0 | u64 seq | u64 tUs (steady clock µs) | payload
```

`len` 最多 16 MiB + 20。`seq` 在进程内递增（回放的帧保留原来的 seq）。

| kind | | payload |
|---|---|---|
| 1 | CLIENTBOUND | 一个游戏数据包：varint 头（`id \| senderSub<<10 \| targetSub<<12`）+ 包体，与 bedrock-protocol 的 `parsePacketBuffer` 读取方式一致 |
| 2 | SERVERBOUND_ECHO | 同上，客户端发出的包（从不包括 id 1、3、4、193） |
| 4 | MARKER | UTF-8 JSON |

读取方忽略其他 kind。flags：1 回放（来自加入日志），2 重新序列化（`Packet::write`，不是线上字节），4 本地连接，8 被抑制（保留）。

| 标记 | 字段 |
|---|---|
| `HELLO`（总是第一个） | `tap:1`、`mod`、`game:"1.26.51.01"`、`protocol`（2193）、`pid`、`link{state, type, address}`、`journal{frames, bytes, complete}`（这个读取方被回放的内容） |
| `LINK_UP` | `type` remote\|local、`address` |
| `LINK_DOWN` | `reason` |
| `REPLAY_BEGIN` | `frames` |
| `REPLAY_END` | `complete`（日志超过 `tapJournalMiB` 时为 false） |
| `DROPPED` | `count`；之后管道关闭 |

读取方依次收到 HELLO、REPLAY_BEGIN、加入日志的帧（带回放标志）、REPLAY_END，然后是实时流；回放期间到达的实时帧等到 REPLAY_END 之后再发，
回放已包含的实时帧按 seq 跳过。没有日志时这一对标记之间为空（`complete:false`）。落后超过 32 MiB 的读取方会收到 DROPPED 并被断开；重新连接会得到完整回放。

所有发布者共用 `src/TapSink.h`，它必须保持逐字节一致：每个发布者用 `prev = gEmit.exchange(&own)` 安装自己的 sink，把每帧转发给 `prev`，
移除时只有 `gEmit` 仍是自己时才放回 `prev`（compare-exchange）。钩子只复制字节；管道的写线程负责清空队列。

## 远程服务器（`NetTap.cpp`）

客户端的 NetworkSystem 跟踪它唯一的出站连接（`$onNewOutgoingConnection`；`$onConnectionClosed`、`$onAllConnectionsClosed`、`closeConnection`
和 `disconnect` 结束它）。在远程连接上，`NetworkConnection::receivePacket`（在它之后，`HasData`）给出每个已解压、已解密的包，
`NetworkSystem::_sendInternal` 给出客户端发出的每个已序列化包：线上字节，flags 为 0。打开配置 `trace` 时，每个连接的前 50 个包会记为
`tap trace #n in|out id len`：它们的顺序与 BDS 抓包一致（network_settings、server_to_client_handshake、play_status、resource_packs_info、……、start_game、……）。

**Blob 缓存。** 打开 blob 缓存的客户端收到的子区块只是 blob 哈希，以及它缺的 blob（`client_cache_miss_response`）；其余来自它磁盘上的缓存，
从不经过网络，读取方就无法构建世界。因此 tap 打开时，BeDD 在客户端的 `client_cache_status` 发出前清掉其中的 `enabled`，服务器就会完整发送每个子区块。
回显里显示 `enabled:false`。

## 单人世界和局域网主机世界（`TapLoopback.cpp`，配置 `tapLoopback`）

`tapLoopback`（默认 `true`，只在 `tap` 打开时有效）把单人世界和客户端开的局域网世界也放到 tap 管道上。这时客户端通过 LocalConnector
与进程内的服务器通信，远程抓取不发布任何东西；改为挂钩 `LoopbackPacketSender`，每个包重新序列化（varint 头 `id | senderSub << 10 | targetSub << 12`，
然后用发送方 `NetworkSystem` 的 `ReflectionCtx` 调 `Packet::write`）。帧带 `reserialized`（2）和 `local`（4）标志。

- **CLIENTBOUND**：服务器的发送器（`$sendToClient` 两个重载、`$sendToClients`、`$sendBroadcast` 两个重载、`$sendTo`），只针对在服务器上连接类型为
  `Local` 的 `NetworkIdentifier`。局域网访客是远程连接，从不发布（计为 `guest`）。广播在本地玩家收到 `start_game` 之后才发布。
- **SERVERBOUND_ECHO**：客户端的发送器（`$sendToServer`、`$send`、`$sendTo`），从不包括 id 1、3、4 或 193。
- 区块：集成服务器通过同一个发送器回应每个 `subchunk_request`，所以客户端交换的每个 `subchunk` 和 `subchunk_request` 都在 tap 上
  （游戏内验证过：`subchunk` 帧数与 `ServerNetworkHandler::handle(SubChunkRequestPacket)` 调用次数相同）。客户端以一个原点（它自己的子区块）
  为基准请求，每列最多到 `highest_subchunk_count`；本地客户端发送的 `client_cache_status` 为 `enabled:false`，所以每项都带数据。
- 只在客户端的连接（`tap::gLinkType`）为本地时。客户端 `NetworkSystem` 上的 `$onNewOutgoingConnection` / `$onConnectionClosed` 按连接类型设置它，
  和 tap 核心一样，所以回环 tap 不依赖 tap 核心也能工作。还不知道连接时（进程内服务器在客户端的连接调用返回前就开始发送），客户端的 `Local`
  连接也算；这个检查从不写 `gLinkType`。远程抓取只在 `Remote` 连接上发布（本地连接也会走 `_sendInternal`），所以没有包会被发布两次。
  一个发送方法调用另一个时，包只发布一次。
- 没有 tap 管道（`tap::gEmit` 为空）时帧只计数。`state` 报告 `stats.tapLoopback{enabled, hooks, link, clientbound, echo, bytes, unpublished, guest}`。
- 打开 `trace` 时，前 50 帧会记入日志（`trace tap loopback: in|out id= len=`），前 4000 帧按 tap 管道的帧格式写入
  `mods\BeDD\data\tap-loopback.bin`（seq 和 tUs 为 0）。

## 加入日志（`TapJournal.cpp`）

`tap` 打开时，日志挂到 `bedd::tap::gEmit` 链上并设置 `bedd::tap::gReplay`，tap 管道对每个新读取方在 `REPLAY_BEGIN` 和 `REPLAY_END` 之间调用它。
回放的帧保持原来的 seq 和顺序，带 `replay` 标志。

- 只保留 CLIENTBOUND 帧。日志在新连接（`LINK_UP` 标记或 `gLinkType` 变化）和 `start_game`（11）时重置，`start_game` 成为它的第一帧。
- 压缩键（只读前导字段；同一个键以最后一帧为准）：`level_chunk`（x、z、维度）、`update_block`（位置、层）、`set_time`、`set_health`、
  `network_chunk_publisher_update`、`current_structure_feature`、`inventory_content`（窗口）、`inventory_slot`（窗口、格子）、`level_event`
  （事件 id，所以天气保持最后的状态）、`update_abilities`（unique id）、各 `add_*` 包（unique id；`add_player` 按 runtime id），以及按 runtime id 的
  `set_entity_motion`、`move_entity`、`move_player` 和 `mob_armor_equipment`。只带变化字段的包按实体和字段集合作键，所以每个字段都保持最后的值：
  `set_entity_data`（元数据键）、`update_attributes`（属性名）、`move_entity_delta`（出现的字段）和 `mob_effect`（效果）。
- `subchunk` 按子区块保留，而不是按包：客户端以一个原点为基准请求所有子区块，所以只有当更新的帧覆盖了一个包里的每个子区块（原点 + 每项的偏移）
  时，这个包才会被丢弃。日志无法遍历的包整个保留。
- `remove_entity`（14）删除该实体的 `add_entity`/`add_item_entity`/`add_player` 帧（`add_player` 按其 unique id 的字节匹配），以及所有按它的 runtime id 作键的帧。
- 不保留：`text`、`animate`、`network_stack_latency`、`play_sound`、`stop_sound`、`level_sound_event`（及 v2）、`spawn_particle_effect`、`level_event_generic`。
- 超过 `tapJournalMiB`（默认 128）后不再保留帧，日志在下次重置前报告 `complete:false`：这时中途接入的读取方会缺区块，直到玩家重新进入这些区块或客户端重新加入。
- `state` 报告 `stats.journal`：`enabled`、`frames`、`bytes`、`complete`，以及计数 `seen`（客户端方向帧）、`compacted`（被同键的后续帧取代）、
  `pruned`（add 帧被 `remove_entity` 删除的实体）、`transient`（不保留）、`overCap`（超过 `tapJournalMiB` 后未保留）、`resets` 和 `replays`。
