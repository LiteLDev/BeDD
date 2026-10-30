# 控制管道协议

PowerShell 客户端（`BeDD.psm1`）只是这个协议的一个客户端；任何能打开命名管道的程序都能以同样方式与游戏通信。
`Invoke-BeDD @{ op = '...' }` 原样发送一个请求。

## 请求与回复

`\\.\pipe\<pipeName>`（仅本机，默认 `BeDD`），UTF-8，每个方向每行一个 JSON 对象。`id` 会原样带回。回复是
`{"ok":true,...}` 或 `{"ok":false,"error":"..."}`。输入类操作在游戏线程分发完输入后才回复（`timeoutMs`，默认 5000）。
协议版本（`ping`.`protocol`）保持为 1：较新的操作通过调用来探测（见下文）。

| op | 字段 | |
|---|---|---|
| `ping` | | `protocol`、`pid`、`hwnd`、`client{w,h}`、`cursor{x,y}`、`realForeground`、`managed` |
| `state` | | `ping` 的字段 + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput, loopbackRebinds, serverQueriesRefused, audio, tapLoopback, send, journal}`、`clients`（当前连接的控制管道客户端数）、`hooks{name: installed}`（各功能安装的每个游戏钩子；功能关闭时为 `false`） |
| `key` | `key`（名字）或 `vk`；`action` press\|down\|up；`holdMs` | 名字：字母、数字、ESC、ENTER、BACK、TAB、SPACE、SHIFT、CTRL、ALT、LEFT/UP/RIGHT/DOWN、HOME、END、PGUP、PGDN、INSERT、DELETE、F1..F24、SLASH、GRAVE、MINUS、EQUALS |
| `text` | `text`；`delayMs` | 每个 UTF-16 单元一个 `WM_CHAR` |
| `mouse` | `action` move\|down\|up\|click\|wheel；`x`,`y`（客户区像素）或 `rx`,`ry`（比例）；`button` left\|right\|middle；`notches`；`holdMs` | 先移动虚拟光标（给了坐标时），再在虚拟 GameInput 鼠标上按按钮/滚轮 |
| `raw` | `action` move\|down\|up\|click\|wheel；`dx`,`dy`（鼠标计数）；`steps`；`button`；`notches`；`holdMs` | 虚拟 GameInput 鼠标：鼠标被锁定时转视角 |
| `release` | | 松开所有按住的键和按钮（不管是谁按的），并结束 `input` 移动覆盖 |
| `focus` | | 重新发送"获得焦点"消息 |
| `window` | `clientWidth`、`clientHeight` | 从不激活窗口；窗口留在屏幕外 |
| `flush` | | 等到所有已投递的输入都分发完 |

## 多个客户端

可以同时连接多个客户端（配置 `pipeInstances`，默认 4；1 = 一次一个），每个由自己的线程服务。投递窗口输入的操作是串行的，
所以两个客户端的按键或输入的文字不会交错；`ping`、`state`、`tap`、`world`、`look`、`input`、`send` 和 `stack` 不加这把锁
（它们只读，或把工作交给客户端线程）。一个客户端断开（或其进程退出）时，释放它持有的东西，且不动其他客户端持有的：
它的按键和鼠标按钮、它发起的 `button` 按住、它设置的 `input` 覆盖。管理窗口通过管理连接转发的操作既不串行，也不归属任何客户端。

对有按客户端状态的单元（`src/BeDD.h`）：`currentPipeClient()` 是当前线程所服务客户端的 id（从 1 开始，不复用；管理连接和游戏线程上为 0）。
用它标记状态，并在 `src/Pipe.cpp` 的 `clientGone()` 里释放（像 `clearMoveInputOf` 和 `releaseGameButtonsOf` 那样），或在 `enable()`
里注册 `onPipeClientGone(fn)`：两者都在离开的客户端线程上运行，在它的按键和按钮释放之后、输入操作串行的情况下（关闭时不运行）。

## 给 bedboo 用的操作（`bedd` 传输）

这些操作在对应配置项打开前都是关闭的，此时回复 `{"ok":false,"code":"disabled","key":"<key>"}`。错误可能带 `code`：
`disabled`、`not_in_level`、`refused`、`timeout`、`bad_args`、`busy`。当前版本没有的操作回复 `unknown op 'x'`。
客户端通过不带参数调用每个操作一次来探测游戏有什么：`unknown op` = 没有，`code:"disabled"` = 有但关着，`ok` = 有。
每个操作的无参数形式都是只读的。

| op | 配置项 | 无参数回复 | 执行形式 |
|---|---|---|---|
| `tap` | `tap` | `enabled`、`pipe`、`attached`（有读取方连接）、`link{state up\|down, type remote\|local, address}`、`journal{frames, bytes, complete}`（当前加入日志）、`lastReplay`（上一个读取方回放了什么）、`replay`（装了日志）、`counters{in, out, dropped, sent, readers}`、`hooks` | （无） |
| `world` | `gameHooks` | `inLevel`、`levelEpoch`（每进入一个世界 +1）、`topScreen`（如 `hud_screen`、`pause_screen`）、`mouseGrabbed`、`showingMenu`、`inputMode` mouse\|touch\|gamepad\|undefined、`player{runtimeId, eye{x,y,z}, rot{yaw,pitch}, slot}`（不在世界里时为 null）、`tick`（见到的客户端刻数） | （无） |
| `look` | `gameHooks` | `{inLevel, yaw, pitch, tick}` | `yaw`、`pitch`、`timeoutMs`（1000，最多 60000） |
| `input` | `gameHooks` | `active`、`via` hooks\|keys、`hooks{raw,state}`、`tick`、`applied` | `move{x,z}`、`jump`、`sneak`、`sprint`、`ttlMs`（300） |
| `slot` | `gameHooks` | `{slot}`（不在世界里时为 null） | `slot` 0..8 |
| `action` | `gameHooks` | `{names}` | `name` drop\|dropAll\|inventory\|perspective\|pause |
| `button` | `gameHooks` | `{held}` | `button` attack\|use\|pick；`action` down\|up\|click；`holdTicks`；`releaseOn` blockChange |
| `send` | `allowSend` | `{allow}`（`sendAllow` 里的 id） | `buf`、`timeoutMs`（2000，最多 10000） |
| `stack` | `allowStack` | `{enabled, via:"raw", stats{sent, answered, rejected, failed, resyncs}}` | `buf`、`timeoutMs`（2000）；或 `view:true` |

- **`world`** 是客户端线程拍的快照：界面字段每帧更新（`ClientInstance::$update` 后置钩子，因为暂停的单人世界不走刻），玩家字段每个客户端刻更新。
  管道线程只复制它。
- **`look`** 在客户端刻里用 `LocalPlayer::localPlayerTurn` 转动玩家，两个角度都在 0.5 度以内时回复 `yaw`、`pitch`、`tick`、`turns`。
  角度与 player_auth_input 一致：yaw 0 = +Z，90 = -X；pitch -90 = 向上，像游戏一样限制在 ±89.9。`localPlayerTurn(d)` 按度 1:1 转动，
  轴互换（`{-dPitch, dYaw}`），转动在请求它的那一刻之后生效。错误：`not_in_level`、`timeout`（带当前 `yaw`/`pitch`；暂停的单人世界不走刻）、
  `busy`（被更新的 `look` 取代）、`bad_args`。
- **`input`** 在游戏自己的输入管线里移动本地玩家，所以每个 `player_auth_input` 都由客户端自己构建。`move` 是 player_auth_input 的移动向量
  （`x` = 左 - 右，`z` = 前 - 后，各自限制在 [-1, 1]；斜向会被归一化）。这个覆盖是一个"死人开关"：请求后 `ttlMs`（最多 60000）结束，
  所以控制方想继续移动时要不断刷新；`ttlMs:0`、`release` 和所属客户端断开会立即结束它。
  - `via:"hooks"`：挂在 `ClientInputUpdateSystem::extractRawHIDInput`（写 `RawMoveInputComponent::mRawInput`/`mRawMove`）和
    `inputHandlerUpdatePlayerState`（在原函数前后写 `MoveInputComponent::mInputState`/`mRawInputState`：`MoveInputState` 标志
    `SneakDown`=0、`JumpDown`=7、`SprintDown`=8、`Up`..`Right`=13..16 以及跳跃/潜行输入边沿 21..26，外加 `mAnalogMoveVector`）的后置钩子。
    只在鼠标被锁定且没有界面显示时生效；与按键绑定无关。键盘处理只在按键事件时改这些标志，所以覆盖结束后的第一刻会把它们清零一次。
    `tick` 计数 `inputHandlerUpdatePlayerState` 的调用，`applied` 计数写入覆盖的刻数。
  - `via:"keys"`（某个钩子没装上）：默认键位的虚拟键（W/A/S/D、SPACE、LSHIFT、LCTRL）按住到截止时间。它们像 `key` 一样经过窗口，
    所以也会到达打开的界面，改过的键位会让它失效。
- **`slot`、`action`、`button`**：管道线程把它们排队，下一个客户端刻执行后回复（`timeoutMs`，默认 1000）：`{"ok":true,"tick":n,...}`，
  或 `code` `not_in_level`、`busy`（有界面打开；`action pause` 和 `button up` 仍可用）、`timeout`、`bad_args`、`refused`。
  暂停的单人世界不走刻：在那里 `action pause` 之后，用 `key` 操作（ESC）恢复。
  - `slot`：`ClientInputCallbacks::handleSlotSelectButtonPress`；它不改格子时（1.26.51 里键盘玩家就是这样）改用默认键位的快捷栏键 `1`..`9`。
    客户端自己发送 `mob_equipment`。回复 `slot` 和 `via` callback\|keys。
  - `action`：对应的 `ClientInputCallbacks::handle*ButtonPress`（背包用 `CraftingType::Inventory`）。
  - `button`：虚拟 GameInput 鼠标上的左/中/右键，由刻监听器在 `holdTicks` 后松开（click 默认 2），或者带
    `releaseOn:"blockChange"` 时，在按下时客户端命中结果指向的方块改变、变成空气或不再被指向时松开（没有指向方块时为 `refused`）。
    `up` 立即生效。从不阻塞管道。回复 `held` 和 `block{x,y,z}`。
- **`send`**：`buf` 是一个服务端方向数据包的 base64（varint 头 + 包体，与 bedrock-protocol 的序列化一致）。它在客户端线程上重建
  （`MinecraftPackets::createPacket` + `readNoHeader`，用客户端 NetworkSystem 的 `ReflectionCtx`），并通过本地玩家发送（`sendNetworkPacket`），
  远程服务器和单人世界都一样；发出后回复 `{packetId, name}`。游戏读不完的字节为 `bad_args`，什么都不发；不在 `sendAllow` 里的 id 为 `refused`。
  `state` 报告 `stats.send{sent, refused, failed, lastError}`。
- **`stack`**：见下文。

## 安全

`send` 让当前用户下任何能打开管道的进程都能让客户端以登录玩家身份发包，所以它默认关闭（`allowSend`），且限于 `sendAllow`
（默认 text 9 和 command_request 77：聊天和命令，和玩家能输入的一样）。id 1（login）、30（inventory_transaction）、144（player_auth_input）
和 147（item_stack_request）即使列出也始终被拒绝：它们归客户端所有，伪造的包会干扰它的预测和记账。只在你控制的服务器上放宽 `sendAllow`。
`allowStack` 打开时，任何能打开管道的程序都能在客户端所在的任何服务器上移动、合成、丢弃和销毁登录玩家的物品。tap 包含登录玩家的整个会话。
除非测试框架需要，三者都保持关闭；管理管道从不打开它们。

## `stack`：通过客户端发送物品堆请求（`allowStack`）

| 形式 | 回复 |
|---|---|
| `{op:"stack"}`（探测） | `{ok, enabled:true, via:"raw", stats:{sent, answered, rejected, failed, resyncs}}` |
| `{op:"stack", buf, timeoutMs=2000}` | `{ok, via:"raw", requestIds:[客户端 id], map:{"<调用方 id>": 客户端 id}}` |
| `{op:"stack", view:true}` | `{ok, inventory:[...], ui:[...], open?:{containerId, type, items:[...]}}`：客户端显示的内容，每项 `{slot, item, count, stackId}` |

- `buf` 是一个完整 item_stack_request 包（id 147）的 base64，varint 头加包体，与 bedrock-protocol 的序列化一致。其他包 id 以及游戏读不完的字节都是 `bad_args`。
- 下一个客户端刻用 `MinecraftPackets::createPacket` 和 `readNoHeader` 重建这个包，由本地玩家发送。
- 请求 id：每个请求获得一个新的客户端 id，包发出后回复。id 是奇数、负数，从 `-0x40000001` 往下数，远低于客户端自己的 `-1, -3, ...`。
  每个指向调用方 id 的格子堆 id 也会改名，包括请求自己的 id（`consume`，或合成产物）以及同一个包或更早包里更早的请求（记住最近 256 个 id）。
  `map` 的键是调用方 id 的字符串形式。
- 服务器在 item_stack_response 里用客户端 id 回答，bedboo 在 tap 上看到它。回答只能在包发出后到达，但可能先于 `stack` 的回复被读到，
  所以调用方要把未匹配的回答保留到回复到达。
- `via:"raw"`：请求不经过客户端的 `ItemStackNetManagerClient`。这个游戏版本里该管理器没有可调用的符号（LeviLamina 头文件里每个成员都是 MCNAPI），
  而且它的预测来自界面控制器移动物品，而不是请求动作。所以客户端不会为这些请求做任何预测。
- 重新同步：服务器接受其中一个请求时（挂钩 `ClientNetworkHandler::handle(ItemStackResponsePacket)`），客户端在下一刻发送
  `inventory_transaction {type: inventory_mismatch}`。这是客户端自己背包不同步时发的包，服务器会回发整个背包。客户端随后显示服务器的状态，
  包括打开的容器。被拒绝的请求两边都不改变，所以不重新同步。
- 服务器会检查每个请求（堆 id、打开的界面、配方）。请求需要的界面（背包、箱子）必须已经在游戏里打开：用 `action inventory`、`key E` 或使用方块打开。

## 谁发什么（bedboo `bedd` 传输）

真实客户端拥有每个服务端方向的数据包；bedboo 把它本来要发的东西变成意图。此表与 bedboo 的 `lib/bedd/authority.js` 相同。

| 类别 | 数据包 | 处理 |
|---|---|---|
| clientOwned | login、client_to_server_handshake、request_network_settings、resource_pack_client_response、client_cache_status、client_cache_blob_status、request_chunk_radius、subchunk_request、serverbound_loading_screen、set_local_player_as_initialized、network_stack_latency、tick_sync、player_skin、emote_list、respawn、player_action（respawn / dimension_change_ack / start_item_use_on / stop_item_use_on）、animate、interact mouse_over_entity | bedboo 丢弃它们（计数）：客户端自己发。重生是用户在客户端上点"重生" |
| translated | player_auth_input（move_vector + 标志 → `input`；yaw/pitch → `look`；block_actions start_break/abort_break/stop_break → `button attack`）、mob_equipment（`slot`）、inventory_transaction（item_use click_block/click_air、item_release、item_use_on_entity：先 `look`，再 `button use`/`attack`）、interact open_inventory（`action inventory`）、container_close（按键 ESC）、item_stack_request（`stack`） | bedboo 的路由把它们变成操作 |
| shared | text、command_request | 通过 `send` 发送；后备：T 或 SLASH，然后 `text`，然后 ENTER |
| unsupported | book_edit、block_pick_request、crafting_event、set_default_game_type，以及其他所有 | bedboo 抛出 BeddUnsupportedError（ERR_BEDD_UNSUPPORTED） |

只有在 `stack` 探测不回答 `ok` 时，item_stack_request 才算 unsupported。

## 管理管道

`\\.\pipe\BeDD-Manager-<会话 id>`（仅当前用户），由 `BeDDManager.ps1` 提供，和控制管道一样是 JSON 行。
`BEDD_MANAGER_PIPE` 可以改用另一个管理窗口的管道（脚本和游戏都读它）。

- **客户端**（每个连接一个请求）：`ping`（`games`；关闭中时 `ok` 为 false）、`adopt` `{pid, instance, label}`（`Start-BeDDGame.ps1` 启动游戏后立即调用）、
  `list`（`games[]`：`pid`、`instance`、`label`、`pipe`、`hwnd`、`linked`、`managed`、`job`、`thumbnail`）、
  `game` `{pid, action: close}`、`shutdown`。
- **游戏**（模块）：第一行是 `hello` `{protocol, pid, hwnd, pipe, instance, label}`（`label` 来自 `BEDD_LABEL`），回复
  `{"ok":true}`（正在关闭的管理窗口会直接挂断；游戏只有被欢迎后才算已连接）。连接保持打开，管理窗口在上面发送请求：每个控制管道操作
  （它每秒轮询一次 `state`），外加 `close`（向游戏窗口发 `WM_CLOSE`）。没有管理窗口的游戏会启动一个（配置 `managerScript`）；
  90 秒内没有管理窗口连上、或者彻底失去连接的游戏会自行关闭。
