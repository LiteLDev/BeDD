# WebSocket 接口

契约：[bedd.asyncapi.yaml](https://docs.levimc.org/BeDD/bedd.asyncapi.yaml)（`spec/`，AsyncAPI 3.0；[Studio](https://studio.asyncapi.com/?url=https://docs.levimc.org/BeDD/bedd.asyncapi.yaml)）。

## 连接

发现文件 `%LOCALAPPDATA%\BeDD\run\<pid>.json`（根目录 `BEDD_HOME`），监听期间存在；跳过已退出的 pid。
`BeDD.exe ps --json` 列出相同内容。

```json
{"protocol": 2, "kind": "client", "pid": 23116, "url": "ws://127.0.0.1:53021/v1", "token": "9c1f…(64 hex)",
 "instance": "MyInstance-2", "instanceDir": "…\\instances\\MyInstance-2", "label": "smoke test", "modVersion": "0.4.0", "startedAt": "2026-11-13T09:41:07Z"}
```

| 项 | |
|---|---|
| `kind` | `client`（游戏）或 `server`（[服务端构建](#服务端目标)） |
| 端口 | 配置 `wsPort`（默认 0：每次启动取空闲端口）；从文件读取 |
| 令牌 | 32 个随机字节，每次启动重新生成。`Authorization: Bearer <token>` 或 `?token=<token>`；错误或缺失：HTTP 401 |
| Origin | 握手带 `Origin` → HTTP 403，除非在配置 `allowOrigins` 中（默认为空）：浏览器无法连接 |
| 限制 | 仅 `127.0.0.1`；`wsMaxClients` 条连接（8）；UTF-8 JSON 文本消息 ≤ 16 MiB；不压缩 |

Node.js 24（其 `WebSocket` 不设请求头）：

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

## 请求与回复

```json
{"id": 7, "op": "look", "yaw": 90, "pitch": 0}
{"id": 7, "ok": true, "yaw": 90, "pitch": 0, "tick": 5182, "turns": 1}
```

- `id`：任意 JSON 值，原样返回。失败：`{id, ok:false, error, code?, key?}`；按 `code` 判断（[错误](#错误)）。
- 每条连接按顺序处理；并行请求用多条连接。
- 探测：不带参数发送操作（bedboo 操作此时只读）：`unknown op` = 不存在，`disabled` = 已关闭，`ok` = 可用。

## 选择模式

先 bedboo 操作，再 `ui`，最后 input 操作（[原因](/zh/guide/operation-modes)）。`ping` 列出它们：

```json
{"ok": true, "protocol": 2, "pid": 23116, "kind": "client", "defaultMode": "bedboo",
 "modes": {"bedboo": ["world","look","input","slot","action","button","settings","send","stack","timescale","join","tap"],
           "ui": ["ui","oreui"], "input": ["key","text","mouse","raw","release","flush","focus","window","screenshot"]},
 "hwnd": 4589122, "managed": true, "client": {"w": 1280, "h": 720}, "cursor": {"x": 640, "y": 360}, "realForeground": false}
```

## bedboo 操作

配置键为 `false` → `disabled`。[实现](/zh/reference/how-it-works#游戏内的-bedboo-操作)。

| 操作 | 配置键 | 探测回复 | 动作字段（默认） |
|---|---|---|---|
| `world` | `gameHooks` | `inLevel`、`levelEpoch`（每进一个世界 +1）、`topScreen`、`mouseGrabbed`、`showingMenu`、`inputMode` mouse\|touch\|gamepad\|undefined、`player{runtimeId, eye{x,y,z}, rot{yaw,pitch}, slot}`（null：无世界）、`tick` | 无 |
| `look` | `gameHooks` | `inLevel, yaw, pitch, tick` | `yaw`、`pitch`、`timeoutMs`（1000\*，≤60000） |
| `input` | `gameHooks` | `active`、`via` hooks\|keys、`hooks{raw,state}`、`tick`、`applied`、`ticksLeft` | `move{x,z}`、`jump`、`sneak`、`sprint`、`ttlMs`（300）或 `ttlTicks` |
| `slot` | `gameHooks` | `slot`（null：无世界） | `slot` 0..8、`timeoutMs`（1000\*） |
| `action` | `gameHooks` | `names` | `name` drop\|dropAll\|inventory\|perspective\|pause\|swing；`hand` main\|off（swing）；`timeoutMs`（1000\*） |
| `button` | `gameHooks` | `held` | `button` attack\|use\|pick；`action` down\|up\|click（click）；`holdTicks`（click：2）；`releaseOn` blockChange；`timeoutMs`（1000\*） |
| `settings` | `gameHooks` | `renderDistance`（区块；第一次进世界前为 null）、`inLevel` | `renderDistance`（区块，1..255） |
| `send` | `allowSend` | `allow`（`sendAllow` 中的 id） | `buf`、`timeoutMs`（2000\*，≤10000） |
| `stack` | `allowStack` | `enabled, via:"raw", stats{sent, answered, rejected, failed, resyncs}` | `buf`、`timeoutMs`（2000\*）；或 `view:true` |
| `tap` | `tap` | `enabled`、`attached`、`link{state up\|down, type remote\|local, address}`、`journal{frames, bytes, complete}`、`lastReplay`、`replay`、`counters{in, out, dropped, sent, readers}`、`hooks` | `action:"subscribe"` → [tap 流](/zh/reference/tap) |
| `timescale` | 无 | [见下](#timescale-游戏速度) | `scale`、`paused`、`timeoutMs`（2000） |
| `join` | 无 | `mode`、`connection` none\|local\|remote、`host`、`port`、`inLevel`、`joining`、`last` | `host`（null：离开世界）、`port`（19132）、`wait`、`timeoutMs`（60000） |

\* 指 ≥20 tick/s 时；更慢时默认值按 tick 间隔 / 50 ms 放大（不超过上限）。`not_in_level`：20 个 tick 间隔（≥1 s）内
没有客户端 tick。

| 操作 | 行为 | 错误 |
|---|---|---|
| `look` | 绝对角度；两者都在 0.5° 内时回复 `yaw, pitch, tick, turns`。Yaw 0 = +Z，90 = -X；pitch -90 = 朝上，限制在 ±89.9 | `not_in_level`、`timeout`（带当前 yaw/pitch）、`busy`（有更新的 `look`）、`bad_args` |
| `input` | `move`：`x` = 左右，`z` = 前后，限制在 [-1,1]，斜向归一化。在请求后 `ttlMs`（≤60000，超出截断）结束（要继续移动就刷新），或遇 `ttlMs:0`、`release` 结束。`ttlTicks`（≤1200）按客户端 tick 计数，5 s 无 tick 也结束。比其连接活得久。`via:"hooks"`：仅在鼠标被捕获且无界面时生效；`applied` = 已写入的 tick 数。`via:"keys"`（钩子缺失）：像 `key` 一样按住 W/A/S/D、SPACE、LSHIFT、LCTRL（会作用到界面，改键后失效） | `bad_args`（`ttlTicks` 与 `ttlMs` 同时给出）、`not_in_level`（`ttlTicks`，无 tick） |
| `slot`、`action`、`button` | 下一个客户端 tick 应用后回复（`tick`）。暂停的单人世界不 tick：用 `key` ESC 恢复 | `not_in_level`、`busy`（界面打开；`button up` 仍可用）、`timeout`、`bad_args`、`refused` |
| `slot` | 快捷栏按键 1..9；选中后回复（`slot`、`via` keys\|none） | |
| `action` | 按键在下一 tick 松开：drop Q、dropAll Ctrl+Q、inventory E、pause Escape（默认键位）；perspective：调用游戏函数。`swing`：`LocalPlayer::swing`，挥手不攻击，客户端自己发 animate；打开界面时也能用；`hand:"off"` 只在本客户端可见。回复 `name`、`hand` | |
| `button` | 虚拟 GameInput 左/右/中键；`holdTicks` 后松开，或（`releaseOn:"blockChange"`）在 `down` 时瞄准的方块改变、变成空气或不再被瞄准时松开。`up` 立即松开。从不阻塞。回复 `held`、`block{x,y,z}`。断开时只松开无终点的 `down`；其余跑完，`releaseOn` 最多再 1200 tick | `refused`：未瞄准方块 |
| `settings` | `renderDistance` 在下一客户端 tick 设置所有渲染器（普通、Vibrant Visuals、光线追踪）的渲染距离（不写入 options.txt），再调用 `ClientInstance::updateChunkRadius`：客户端发 request_chunk_radius；服务器批准的半径以 chunk_radius_update 到达（tap）。回复 `renderDistance`（当前渲染值）、`requested`、`tick` | `not_in_level`、`timeout`、`bad_args`、`unavailable` |
| `send` | `buf`：base64 的服务端方向数据包（varint 头 + 包体，bedrock-protocol），由本地玩家发送；回复 `packetId, name`。统计：`state.stats.send{sent, refused, failed, lastError}` | `bad_args`（无法解析，未发送）、`refused`（不在 `sendAllow` 中） |
| `join` | 从标题界面或世界中（先离开：`left`）；自动应答 "Online play is not rated"（Proceed）和 "Trust this server?"（Trust and Join）。开始连接后 `started:true`；`wait:true` → `joined:true`。非本机主机需要 `allowHosts`（Remote 模式）。`host: null` 离开世界（保存并退出 / 断开连接）：`left:true`（`wait`：存好并回到菜单后；否则 `started:true`），不在世界中时 `left:false` | `join_failed`、`timeout`（`screen`）、`busy`、`not_allowed`、`stopped`、`no_client`、`unavailable`、`bad_args` |

### `stack`：通过客户端发送物品堆请求

| 形式 | 回复 |
|---|---|
| `{op:"stack", buf}` | 发送后 `{via:"raw", requestIds, map:{"<caller id>": client id}}` |
| `{op:"stack", view:true}` | `{inventory, ui, open?:{containerId, type, items}}`，条目 `{slot, item, count, stackId}` |

- `buf`：一个 item_stack_request 数据包（id 147）；其他 id 或无法解析的字节：`bad_args`。
- 每个请求分配一个客户端 id：奇数、负数，从 `-0x40000001` 递减。引用调用方 id 的 stack id 会被改写（本请求和此前
  最近 256 个）。`map` 的键：字符串形式的调用方 id。
- item_stack_response（tap）可能早于回复到达：先保留。
- 无客户端预测。请求被接受：客户端发送 `inventory_transaction {type: inventory_mismatch}`，服务器重发背包。被拒绝：
  无变化。
- 必须打开所需界面（背包、箱子）：`action inventory`、`key E` 或使用方块。

### `timescale`：游戏速度

本游戏托管的世界（单人、局域网主机）的速度：客户端与本地服务端一起调整。请求（`scale`、`paused`）在两侧都以该速度
运行后回复探测内容；探测还含 `local`、`inLevel`。

| 字段 | |
|---|---|
| `scale` | 0.05..20，超出截断（1 = 20 tick/s） |
| `paused` | 两侧一起停止/恢复，同暂停菜单 |
| `source` | `default`、`config`（`timeScale`，进入世界时）、`local`（本接口）、`server`（BeDD 服务端） |
| `tps{client, server}` | 最近 2 s 的 tick/s；暂停为 0；远程服务器上 `server` 为 null。本地实测：0.05→1、0.5→10、2→40、3→60、5→100、10→200、20→400 |
| `ticksPerSecond` | 客户端计时器基准速率（20） |
| `effective{client, server}` | 每侧最近一次应用的 `{scale, paused, ticksPerSecond}`（此前为 null） |

离开世界重置为 1；进入世界应用 `timeScale`（1）。错误：`not_in_level`、`remote`（不在本机托管）、`server_led`（由
BeDD 服务端设置：到那边改）、`timeout`（之后仍会生效）、`refused`、`bad_args`。BeDD 服务端在变更时和玩家加入时发送
level_event 3007（SimTimeScale，pos 0,0,0，`data` = float 位模式按 int32，0 = 暂停）；客户端跟随（0 暂停，保留 scale）。

### 谁发什么（bedboo 传输 `bedd`）

所有服务端方向数据包由客户端发送。bedboo 的 `lib/bedd/authority.js` 丢弃客户端自有的数据包，把其他数据包映射到操作
（`input`、`look`、`slot`、`button`、`action`、`settings`、`stack`、`join`、key ESC），用 `send` 发送 text 和 command_request
（后备：T/SLASH、`text`、ENTER），通过 `ui` 操作离开床和告示牌编辑界面，其他 `sendAllow` 中的 id 用 `send` 原样发送，
其余抛出 ERR_BEDD_UNSUPPORTED（BeddUnsupportedError）。

## ui 操作

`ui` 把界面读成带 ref 的文本并对 ref 执行动作；`oreui` 返回原始 Ore UI DOM；`mouse` 可带 `ref`。配置键 `ui` 控制这三者。
字段、回复和错误：[UI 树](/zh/reference/ui)。

## input 操作

无配置键。游戏线程派发输入后回复（`timeoutMs` 5000）。

| 操作 | 字段（默认） | |
|---|---|---|
| `key` | `key` 或 `vk`（1..254）；`action` press\|down\|up（press）；`holdMs`（60） | 字母、数字、ESC、ENTER、BACK、TAB、SPACE、SHIFT、CTRL、ALT、LEFT/UP/RIGHT/DOWN、HOME、END、PGUP、PGDN、INSERT、DELETE、F1..F24、SLASH、GRAVE、MINUS、EQUALS |
| `text` | `text`、`delayMs`（0） | 每个 UTF-16 单元一条 `WM_CHAR` |
| `mouse` | `action` move\|down\|up\|click\|wheel（move）；`x`、`y`（客户区像素）或 `rx`、`ry`（比例）或 [`ref`](/zh/reference/ui#引用上的-mouse)；`button` left\|right\|middle（left）；`notches`（1）；`holdMs`（80，≥50）；`settleMs`（50，先悬停）；`afterMs`（50） | 移动虚拟光标，然后发 GameInput 按键/滚轮 |
| `raw` | 同 `mouse`，另有 `dx`、`dy`（计数）、`steps`（1，分摊到多帧） | 鼠标被捕获时转动视角 |
| `release` | | 松开所有按住的键/按钮（任何连接的）和 `input` 覆盖 |
| `focus` | | 重发“获得焦点” |
| `window` | `clientWidth`、`clientHeight` | 调整大小；回复 `result` + 窗口字段；从不激活 |
| `flush` | | 等待所有已投递的输入派发完成 |
| `screenshot` | `format` png\|jpeg、`quality`、`maxWidth`（960）、`region{x,y,w,h}` | `{width, height, format, data}`（base64） |

## 系统操作

| 操作 | 回复 |
|---|---|
| `ping` | [示例](#选择模式)；窗口出现前 `hwnd` 为 0，`managed` = 已有管理器附加 |
| `state` | 窗口字段、`stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput, loopbackRebinds, serverQueriesRefused, audio, tapLoopback, send, journal, sendsRefused}`、`clients`、`held[{vk, client}]`、`hooks{name: installed}`、`mode` ClientOnly\|ClientServer\|Remote、`connection{connection none\|local\|remote, inLevel, host, port}` |
| `manager` | `action:"attach"`：本连接成为管理器；窗口保持在屏幕外，不出现在任务栏和 Alt+Tab。配置 `manager`（true）：窗口出现后 30 s 内无人附加，或已附加的断开，游戏即关闭；`false`：隐藏，从不自行关闭。标签：环境变量 `BEDD_LABEL` |
| `close` | `{ok}`，然后游戏关闭 |

## 错误

| code | 含义 |
|---|---|
| `disabled` | 配置键为 `false`（`key`），或正在卸载 |
| `not_in_level` | 不在世界中（服务端：未启动） |
| `refused` | 游戏拒绝（id 不在 `sendAllow` 中，`releaseOn` 无方块） |
| `timeout` | `timeoutMs` 内未完成（之后可能完成） |
| `bad_args` | 字段值无效或字节无法解析 |
| `bad_request` | 操作无法处理：二进制消息；`tap`/`manager` 未知 action 或无连接；`ui` 动作缺 `ref`、未知 `action`、`setText` 缺 `text` |
| `internal` | `ui` 动作的子操作回复无法解析（bug） |
| `busy` | 界面打开、有更新的 `look`、join 进行中、tap 已被占用 |
| `stopped` | 正在卸载 |
| `join_failed` | join 放弃（客户端显示原因） |
| `not_allowed` | 非本机主机、`loopbackOnly`、不在 `allowHosts` 中 |
| `unavailable` | 所需钩子未安装（见游戏日志） |
| `no_client` | 尚无客户端实例 |
| `remote` | 世界在非本机托管的服务器上 |
| `server_led` | 速度由 BeDD 服务端主导 |
| `stale_ref`、`occluded`、`not_ready`、`unsupported` | [UI 树错误](/zh/reference/ui#错误) |

无 `code`：未知操作、不是 JSON 对象、输入未及时派发、名称无效（key、button）。

## 多个客户端

窗口输入类操作在各连接间串行；`ping`、`state`、`manager`、`close`、`tap`、`world`、`look`、`input`、`settings`、`send`、`stack`、
`timescale`、`join`、`screenshot`、`oreui` 和 `ui` 读取不串行。断开时：

| 按住的 | |
|---|---|
| `key`/`mouse` `down` | 松开（除非另一条连接重新按下） |
| `button` | 见上文 |
| `input` 覆盖 | 运行到截止时间 |
| 管理器 | 游戏关闭 |

模组中的每连接状态：[代码规范](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md#code-rules)。

## 安全

能读发现文件的人（该用户的进程）都能驱动游戏。令牌按常数时间比较。

- `send`：以已登录玩家身份发包，限于 `sendAllow`（默认 9 text、77 command_request、78 command_block_update、97 book_edit）。id 1、30、144、147 始终拒绝。
  只为自己的服务器放宽。
- `stack`：在任何服务器上移动、合成、丢弃和销毁玩家的物品。
- tap：整个会话。

关闭不需要的 `allowSend`、`allowStack`、`tap`；不要分享令牌。

## 服务端目标

`--target_type=server`（[构建](/zh/reference/building)）在装有 LeviLamina 的 BDS 中运行：同样的 WebSocket，
`kind:"server"`，其他操作回复 `unknown op`。

| 操作 | 字段（默认） | 回复 |
|---|---|---|
| `ping` | | `protocol`、`pid`、`kind`、`target:"server"` |
| `state` | | `target`、`clients`、`loopbackOnly`、`stats{loopbackRebinds, serverQueriesRefused}` |
| `server` | | `tick`、`tps`（2 s）、`paused`、`port`（`server-port`）、`players[{name, xuid}]` |
| `timescale` | `scale` 0.05..20、`paused`、`timeoutMs`（5000）；都不给 = 探测 | `scale`、`paused`、`source` default\|config\|local、`tps{client:null, server}`、`ticksPerSecond`（20 × scale，暂停为 0） |
| `command` | `command`（`/` 可省略）、`timeoutMs`（10000） | `success`、`successCount`、`output[]`，同控制台 |

变更时及每个玩家生成时广播 level_event 3007（见上）。`tps.server` 可能滞后于 `ticksPerSecond`。错误：`not_in_level`、
`bad_args`、`timeout`。
