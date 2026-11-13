# WebSocket 接口

每个运行 BeDD 的游戏都在 `127.0.0.1` 上提供一个 WebSocket。测试脚本、智能体、[bedboo](https://github.com/LiteLDev/bedboo)
和 BeDD 管理器都通过它驱动游戏，任何能打开 WebSocket 的程序也都能这样做。本页是给人读的参考；机器可读的契约是 AsyncAPI 文档
[bedd.asyncapi.yaml](https://docs.levimc.org/BeDD/bedd.asyncapi.yaml)（即仓库里的 `spec/bedd.asyncapi.yaml`），每个请求和回复都附有
JSON Schema。可以在 [AsyncAPI Studio](https://studio.asyncapi.com/?url=https://docs.levimc.org/BeDD/bedd.asyncapi.yaml) 里浏览，
也可以用它生成客户端。

## 连接

### 找到游戏

WebSocket 服务开始监听后，游戏会写一个发现文件 `%LOCALAPPDATA%\BeDD\run\<pid>.json`（设置了 `BEDD_HOME` 时以它为根目录），
退出时删除。进程已不存在的文件是过期的，跳过即可。

```json
{
  "protocol": 2,
  "kind": "client",
  "pid": 23116,
  "url": "ws://127.0.0.1:53021/v1",
  "token": "9c1f…64 个十六进制字符…",
  "instance": "MyInstance-2",
  "instanceDir": "C:\\Users\\me\\AppData\\Local\\BeDD\\instances\\MyInstance-2",
  "label": "smoke test",
  "modVersion": "2.0.0",
  "startedAt": "2026-11-13T09:41:07Z"
}
```

`kind` 对游戏是 `client`，对运行服务端版本的 Bedrock 专用服务器是 `server`。`BeDD.exe ps --json` 列出同样的端点。
端口是配置 `wsPort`（默认 0：每次启动由系统挑一个空闲端口），所以总是从文件里读。令牌是 32 个随机字节，每次启动都会换。

### 认证与 Origin 规则

- 在握手时用 `Authorization: Bearer <token>` 发送令牌；不能设置请求头的客户端可以在 URL 里带 `?token=<token>`。
  令牌缺失或错误时返回 HTTP 401。
- 握手里带有 `Origin` 头的请求一律返回 HTTP 403，除非该来源列在配置 `allowOrigins` 里（默认为空）。浏览器总会发送 `Origin`，
  所以网页无法连到游戏；脚本和普通程序不会发送它。
- 服务只绑定 `127.0.0.1`，同时最多接受 `wsMaxClients` 个连接（默认 8）。
- 文本消息是 UTF-8 JSON，每条最多 16 MiB。不支持扩展（不压缩）。

### Node.js 24

Node 24 自带全局 `WebSocket`；它不能设置请求头，所以把令牌放在 URL 里。

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

使用 [websockets](https://pypi.org/project/websockets/) 包：

```python
import asyncio, glob, json, os, websockets

def find_game():
    root = os.environ.get("BEDD_HOME") or os.path.join(os.environ["LOCALAPPDATA"], "BeDD")
    for path in glob.glob(os.path.join(root, "run", "*.json")):
        with open(path, encoding="utf-8") as f:
            ep = json.load(f)
        if ep["kind"] == "client":
            return ep  # 实际代码里要检查 ep["pid"] 是否还活着

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
    .First(e => (string?)e["kind"] == "client"); // 实际代码里要检查 pid 是否还活着

using var ws = new ClientWebSocket();
ws.Options.SetRequestHeader("Authorization", $"Bearer {game["token"]}");
await ws.ConnectAsync(new Uri((string)game["url"]!), CancellationToken.None);

await ws.SendAsync(Encoding.UTF8.GetBytes("""{"id":1,"op":"ping"}"""), WebSocketMessageType.Text, true, default);
var buffer = new byte[1 << 20];
var result = await ws.ReceiveAsync(buffer, default); // 较大的回复可能分成多帧
Console.WriteLine(Encoding.UTF8.GetString(buffer, 0, result.Count));
```

## 请求与回复

每条文本消息是一个 JSON 对象。请求用 `op` 指明操作，可以带一个 `id`（任意 JSON 值），回复会原样带回：

```json
{"id": 7, "op": "look", "yaw": 90, "pitch": 0}
{"id": 7, "ok": true, "yaw": 90, "pitch": 0, "tick": 5182, "turns": 1}
```

失败时回复 `{"id", "ok": false, "error", "code"?, "key"?}`：`error` 是给人看的小写英文，程序应该匹配 `code`（见[错误](#错误)）。
同一个连接按顺序处理：游戏回复完一个请求，才会读这个连接上的下一个请求。想并行执行请求（比如一边等很久的 `join`，一边轮询 `world`），
就再开一个连接。

`ping` 回答 `protocol: 2`。客户端通过不带参数调用每个操作一次来探测游戏有什么：`unknown op` = 没有，`code:"disabled"` = 有但在配置里关掉了，
`ok` = 有。每个 bedboo 操作的无参数形式都是只读的。

## 选择模式

BeDD 提供三种驱动游戏的方式，全部同时可用。按这个顺序选：

1. **bedboo**（`defaultMode`）：基于游戏自己的输入管线和网络会话的语义操作。`world` 告诉你玩家在哪、显示着什么界面；
   `look`、`input`、`slot`、`action` 和 `button` 用来玩；`send` 和 `stack` 用来聊天、执行命令和搬物品；`join` 和 `timescale` 用来布置会话。
   输入输出都是小段 JSON，与窗口大小无关，bedboo 用的就是它。
2. **ui**：把游戏界面当作一棵类似无障碍树的组件树（`ui`）。先读一个快照，再按 `ref` 点击、输入文字或滚动某个节点。
   bedboo 操作够不着的菜单和界面用它。
3. **input**：模拟人用键盘鼠标的操作（`key`、`text`、`mouse`、`raw`），外加 `screenshot`。最后才用它：一张截图花掉智能体的 token
   远多于一份 JSON 快照，而且像素坐标一改布局就失效。

`ping` 列出每种模式的操作：

```json
{"ok": true, "protocol": 2, "pid": 23116, "defaultMode": "bedboo",
 "modes": {"bedboo": ["world","look","input","slot","action","button","send","stack","timescale","join","tap"],
           "input": ["key","text","mouse","raw","release","flush","focus","window","screenshot"],
           "ui": ["ui","oreui"]},
 "hwnd": 4589122, "managed": true, "client": {"w": 1280, "h": 720}, "cursor": {"x": 640, "y": 360},
 "realForeground": false}
```

## 系统操作

| op | 字段 | 回复 |
|---|---|---|
| `ping` | | `protocol`（2）、`pid`、`defaultMode`、`modes`、`hwnd`、`client{w,h}`、`cursor{x,y}`、`realForeground`、`managed` |
| `state` | | `ping` 的窗口字段 + `stats{synthetic, swallowedInput, swallowedFocus, passed, realForegroundSeen, api, gameInput, loopbackRebinds, serverQueriesRefused, audio, tapLoopback, send, journal, sendsRefused}`、`clients`（当前连接的 WebSocket 客户端数）、`hooks{name: installed}`（各功能安装的每个游戏钩子；功能关闭时为 `false`）、`mode`（ClientOnly\|ClientServer\|Remote，配置 `mode`）、`connection{connection none\|local\|remote, inLevel, host, port}`（客户端在哪里玩） |
| `manager` | `action:"attach"` | `{ok}`：这个连接成为游戏的管理器 |
| `close` | | `{ok}`，然后游戏关闭 |

**管理器。** BeDD 管理器会连到每个游戏并发送 `{op:"manager", action:"attach"}`。有连接附着时，游戏窗口留在屏幕外、不出现在任务栏和
Alt+Tab 里。配置 `manager`（默认 `true`）打开时，窗口出现后 30 秒内没有管理器附着，或者附着的连接断开，游戏就会自行关闭。
`manager:false` 时窗口保持隐藏，游戏也从不自行关闭（供不经管理器运行游戏的脚本使用）。管理器显示的标签来自游戏的环境变量 `BEDD_LABEL`；
`BEDD_RETURN_FOCUS` 仍然会在启动后把焦点还回去。

## bedboo 操作

下面每个操作都受一个配置项控制，默认为 `true`；配置项设为 `false` 时，操作回复 `{"ok":false,"code":"disabled","key":"<key>"}`。

| op | 配置项 | 无参数回复 | 执行形式 |
|---|---|---|---|
| `world` | `gameHooks` | `inLevel`、`levelEpoch`（每进入一个世界 +1）、`topScreen`（如 `hud_screen`、`pause_screen`）、`mouseGrabbed`、`showingMenu`、`inputMode` mouse\|touch\|gamepad\|undefined、`player{runtimeId, eye{x,y,z}, rot{yaw,pitch}, slot}`（不在世界里时为 null）、`tick`（见到的客户端刻数） | （无） |
| `look` | `gameHooks` | `{inLevel, yaw, pitch, tick}` | `yaw`、`pitch`、`timeoutMs`（1000\*，最多 60000） |
| `input` | `gameHooks` | `active`、`via` hooks\|keys、`hooks{raw,state}`、`tick`、`applied`、`ticksLeft`（`ttlTicks` 覆盖） | `move{x,z}`、`jump`、`sneak`、`sprint`、`ttlMs`（300）或 `ttlTicks` |
| `slot` | `gameHooks` | `{slot}`（不在世界里时为 null） | `slot` 0..8 |
| `action` | `gameHooks` | `{names}` | `name` drop\|dropAll\|inventory\|perspective\|pause |
| `button` | `gameHooks` | `{held}` | `button` attack\|use\|pick；`action` down\|up\|click；`holdTicks`；`releaseOn` blockChange |
| `send` | `allowSend` | `{allow}`（`sendAllow` 里的 id） | `buf`、`timeoutMs`（2000\*，最多 10000） |
| `stack` | `allowStack` | `{enabled, via:"raw", stats{sent, answered, rejected, failed, resyncs}}` | `buf`、`timeoutMs`（2000\*）；或 `view:true` |
| `tap` | `tap` | `enabled`、`attached`（有订阅方连接）、`link{state up\|down, type remote\|local, address}`、`journal{frames, bytes, complete}`（当前加入日志）、`lastReplay`（上一个订阅方回放了什么）、`replay`（装了日志）、`counters{in, out, dropped, sent, readers}`、`hooks` | `action:"subscribe"`：这个连接以二进制消息接收 [tap](/zh/reference/tap) |
| `timescale` | （无） | 见下文 | `scale`、`paused`、`timeoutMs`（2000） |
| `join` | （无） | `mode`、`connection` none\|local\|remote、`host`、`port`、`inLevel`、`joining`、`last` | `host`、`port`（19132）、`wait`、`timeoutMs`（60000） |

\* 以及 `slot`、`action`、`button` 的 `timeoutMs`（1000）：这些默认值适用于每秒 20 刻及更快。客户端走刻更慢时（游戏速度更慢），
不带 `timeoutMs` 的请求相应地等得更久（默认值乘以实测刻间隔除以 50 ms，最多到该操作的上限）；调用方给出的 `timeoutMs` 原样使用。
同样，“客户端在世界里”（`slot`、`action`、`button`、`send`、`stack` 的 `not_in_level`）指 20 个刻间隔（至少 1 秒）内有过客户端刻。

- **`world`** 是客户端线程拍的快照：界面字段每帧更新（`ClientInstance::$update` 后置钩子，因为暂停的单人世界不走刻），玩家字段每个客户端刻更新。
  连接线程只复制它。
- **`look`** 在客户端刻里用 `LocalPlayer::localPlayerTurn` 转动玩家，两个角度都在 0.5 度以内时回复 `yaw`、`pitch`、`tick`、`turns`。
  角度与 player_auth_input 一致：yaw 0 = +Z，90 = -X；pitch -90 = 向上，像游戏一样限制在 ±89.9。`localPlayerTurn(d)` 按度 1:1 转动，
  轴互换（`{-dPitch, dYaw}`），转动在请求它的那一刻之后生效。错误：`not_in_level`、`timeout`（带当前 `yaw`/`pitch`；暂停的单人世界不走刻）、
  `busy`（被更新的 `look` 取代）、`bad_args`。
- **`input`** 在游戏自己的输入管线里移动本地玩家，所以每个 `player_auth_input` 都由客户端自己构建。`move` 是 player_auth_input 的移动向量
  （`x` = 左 - 右，`z` = 前 - 后，各自限制在 [-1, 1]；斜向会被归一化）。这个覆盖是一个“死人开关”：请求后 `ttlMs`（最多 60000）结束，
  所以控制方想继续移动时要不断刷新；`ttlMs:0`、`release` 和所属连接关闭会立即结束它。`ttlTicks`（最多 1200；不能与 `ttlMs` 同时给，否则 `bad_args`）
  改为在那么多个客户端刻之后结束，在任何游戏速度下都是同样的游戏时间；客户端停止走刻 5 秒（离开世界、暂停）时它也会结束，不走刻时请求它会得到 `not_in_level`，探测回复用 `ticksLeft` 报告剩余刻数。
  - `via:"hooks"`：挂在 `ClientInputUpdateSystem::extractRawHIDInput`（写 `RawMoveInputComponent::mRawInput`/`mRawMove`）和
    `inputHandlerUpdatePlayerState`（在原函数前后写 `MoveInputComponent::mInputState`/`mRawInputState`：`MoveInputState` 标志
    `SneakDown`=0、`JumpDown`=7、`SprintDown`=8、`Up`..`Right`=13..16 以及跳跃/潜行输入边沿 21..26，外加 `mAnalogMoveVector`）的后置钩子。
    只在鼠标被锁定且没有界面显示时生效；与按键绑定无关。键盘处理只在按键事件时改这些标志，所以覆盖结束后的第一刻会把它们清零一次。
    `tick` 计数 `inputHandlerUpdatePlayerState` 的调用，`applied` 计数写入覆盖的刻数。
  - `via:"keys"`（某个钩子没装上）：默认键位的虚拟键（W/A/S/D、SPACE、LSHIFT、LCTRL）按住到截止时间。它们像 `key` 一样经过窗口，
    所以也会到达打开的界面，改过的键位会让它失效。
- **`slot`、`action`、`button`** 先排队，下一个客户端刻执行后回复（`timeoutMs`，默认 1000）：`{"ok":true,"tick":n,...}`，
  或 `code` `not_in_level`、`busy`（有界面打开；`button up` 仍可用）、`timeout`、`bad_args`、`refused`。
  暂停的单人世界不走刻：在那里 `action pause` 之后，用 `key` 操作（ESC）恢复。
  - `slot`：默认键位的快捷栏键 `1`..`9`，玩家拿上这一格后回复。客户端自己发送 `mob_equipment`。回复 `slot` 和 `via` keys\|none（本来就拿着）。
  - `action`：默认键位的按键，下一刻松开：drop Q、dropAll Ctrl+Q、inventory E、pause Escape；perspective 调用
    `ClientInputCallbacks::handleTogglePerspectiveButtonPress`（从 1.26.60 起游戏不再导出其他 `handle*ButtonPress`）。改过的键位会让它们失效。
  - `button`：虚拟 GameInput 鼠标上的左/中/右键，由刻监听器在 `holdTicks` 后松开（click 默认 2），或者带
    `releaseOn:"blockChange"` 时，在按下时客户端命中结果指向的方块改变、变成空气或不再被指向时松开（没有指向方块时为 `refused`）。
    `up` 立即生效。从不阻塞连接。回复 `held` 和 `block{x,y,z}`。
- **`send`**：`buf` 是一个服务端方向数据包的 base64（varint 头 + 包体，与 bedrock-protocol 的序列化一致）。它在客户端线程上重建
  （`MinecraftPackets::createPacket` + `Packet::read`，用客户端 NetworkSystem 的 `ReflectionCtx`），并通过本地玩家发送（`sendNetworkPacket`），
  远程服务器和单人世界都一样；发出后回复 `{packetId, name}`。游戏读不完的字节为 `bad_args`，什么都不发；不在 `sendAllow` 里的 id 为 `refused`。
  `state` 报告 `stats.send{sent, refused, failed, lastError}`。
- **`join`** 让客户端通过游戏自己的外部服务器加入流程（`ClientInstance::connectToThirdPartyServer`）按地址加入服务器，可以在标题界面，
  也可以在世界里（先退出世界）。从 1.26.60 起它像玩家用键盘一样回答挡路的两个提示：“在线游戏未分级”（继续，然后重新连接）和针对所加入服务器的
  “信任此服务器？”（信任并加入）。开始连接后回复 `started:true`，带 `wait:true` 时进入世界后回复 `joined:true`；`left`：先退出了一个世界。
  错误码：`join_failed`（客户端放弃了，界面上显示原因）、`timeout`（`screen`：游戏可能还在连接）、`busy`（已有加入在进行）、
  `not_allowed`（`loopbackOnly` 不让游戏访问的本机以外主机：不在 `allowHosts` 里）、`stopped`、`no_client`、`unavailable`、`bad_args`。
  本机以外的服务器需要 Remote 模式（配置 `allowHosts`）。
- **`tap`**：`{op:"tap", action:"subscribe"}` 回答 `{ok:true}`，此后这个连接每收到一条二进制消息就是一帧 tap：HELLO、加入日志的回放，
  然后是实时流。同一时间只有一个订阅方（其他人得到 `busy`）；落后超过 32 MiB 的订阅方收到 DROPPED，连接随之关闭。
  已订阅的连接上不要再发别的请求。帧格式见 [Tap 协议](/zh/reference/tap)。

### `stack`：通过客户端发送物品堆请求

| 形式 | 回复 |
|---|---|
| `{op:"stack"}`（探测） | `{ok, enabled:true, via:"raw", stats:{sent, answered, rejected, failed, resyncs}}` |
| `{op:"stack", buf, timeoutMs=2000}` | `{ok, via:"raw", requestIds:[客户端 id], map:{"<调用方 id>": 客户端 id}}` |
| `{op:"stack", view:true}` | `{ok, inventory:[...], ui:[...], open?:{containerId, type, items:[...]}}`：客户端显示的内容，每项 `{slot, item, count, stackId}` |

- `buf` 是一个完整 item_stack_request 包（id 147）的 base64，varint 头加包体，与 bedrock-protocol 的序列化一致。其他包 id 以及游戏读不完的字节都是 `bad_args`。
- 下一个客户端刻用 `MinecraftPackets::createPacket` 和 `Packet::read` 重建这个包，由本地玩家发送。
- 请求 id：每个请求获得一个新的客户端 id，包发出后回复。id 是奇数、负数，从 `-0x40000001` 往下数，远低于客户端自己的 `-1, -3, ...`。
  每个指向调用方 id 的格子堆 id 也会改名，包括请求自己的 id（`consume`，或合成产物）以及同一个包或更早包里更早的请求（记住最近 256 个 id）。
  `map` 的键是调用方 id 的字符串形式。
- 服务器在 item_stack_response 里用客户端 id 回答，bedboo 在 tap 上看到它。回答只能在包发出后到达，但可能先于 `stack` 的回复被读到
  （它在另一个连接上），所以调用方要把未匹配的回答保留到回复到达。
- `via:"raw"`：请求不经过客户端的 `ItemStackNetManagerClient`。这个游戏版本里该管理器没有可调用的符号（LeviLamina 头文件里每个成员都是 MCNAPI），
  而且它的预测来自界面控制器移动物品，而不是请求动作。所以客户端不会为这些请求做任何预测。
- 重新同步：服务器接受其中一个请求时（挂钩 `ClientNetworkHandler::handle(ItemStackResponsePacket)`），客户端在下一刻发送
  `inventory_transaction {type: inventory_mismatch}`。这是客户端自己背包不同步时发的包，服务器会回发整个背包。客户端随后显示服务器的状态，
  包括打开的容器。被拒绝的请求两边都不改变，所以不重新同步。
- 服务器会检查每个请求（堆 id、打开的界面、配方）。请求需要的界面（背包、箱子）必须已经在游戏里打开：用 `action inventory`、`key E` 或使用方块打开。

### `timescale`：游戏速度

不受配置项控制。它改变本游戏主持的世界（单人世界，或它主持的局域网世界）的速度：客户端和它的本地服务器一起改。

| 形式 | 回复 |
|---|---|
| `{op:"timescale"}`（探测） | `{ok, scale, paused, source, tps:{client, server}, ticksPerSecond, effective:{client, server}, local, inLevel}` |
| `{op:"timescale", scale?, paused?, timeoutMs=2000}` | 与探测相同的字段，在客户端和本地服务器都以它运行之后回复 |

- `scale` 限制在 0.05..20（1 = 正常，每秒 20 刻）。`paused:true` 让两边都停止刻（`Minecraft::setSimTimePause`），`false` 恢复。
  游戏自己的暂停菜单也以同样方式暂停和恢复。
- `source`：`default`（1，未设置）、`config`（`timeScale`，进入世界时应用）、`local`（通过本接口设置）、`server`（运行 BeDD 的服务器设置的）。
- `tps.client`、`tps.server`：客户端和本地服务器每秒的刻数，按最近 2 秒测量（所以比改动滞后至多 2 秒；暂停时为 0）。
  远程服务器上 `tps.server` 为 `null`。`ticksPerSecond`：客户端计时器的基准速率（20）。`effective.client` / `effective.server`：
  各自的 Minecraft 上次应用改动后报告的 `{scale, paused, ticksPerSecond}`（第一次之前为 `null`）。
- 每一边在自己的线程上修改：服务器通过 `ServerInstance::queueForServerThread`，客户端在下一次 `ClientInstance::update` 之后
  （刻暂停时画面帧仍在继续）。速度直接写入模拟计时器（`Timer::mTimeScale`）：`Minecraft::setSimTimeScale` 会把当前速度相乘并封顶为 10，
  不是幂等的。计时器每帧只追赶有限的刻数，所以 `tps` 显示游戏实际达到的速度。本地世界中客户端与服务器一致：
  0.05 → 1、0.5 → 10、2 → 40、3 → 60、5 → 100、10 → 200、20 → 400 刻每秒；再次设置相同速度不会有变化。
- 离开世界时客户端回到 1；进入世界时应用 `timeScale`（默认 1），所以上一个世界的设置不会带过来。
- 错误：`not_in_level`；`remote`（世界在本游戏没有主持的服务器上：无法从客户端改它的速度）；`server_led`（该服务器运行 BeDD 并已发来它的速度：
  到那边去改）；`timeout`（某一边没有及时应用：它的线程运行时仍会应用，`effective` 会显示）；`refused`；`bad_args`。
- 服务器主导：运行 BeDD 的服务器在速度或暂停改变时以及玩家加入时，给每个玩家发送 level_event 包（`event_id` 3007 SimTimeScale，位置 0,0,0，
  `data` = 速度的 float 位按 int32，服务器暂停时为 0）。客户端应用它（挂钩 `ClientNetworkHandler::handle(LevelEventPacket)`，在游戏自己处理之后，
  且只在游戏没有自己达到该速度时）；0 让客户端暂停，并保留原速度供恢复时使用。

### 谁发什么（bedboo `bedd` 传输）

真实客户端拥有每个服务端方向的数据包；bedboo 把它本来要发的东西变成意图。此表与 bedboo 的 `lib/bedd/authority.js` 相同。

| 类别 | 数据包 | 处理 |
|---|---|---|
| clientOwned | login、client_to_server_handshake、request_network_settings、resource_pack_client_response、client_cache_status、client_cache_blob_status、request_chunk_radius、subchunk_request、serverbound_loading_screen、set_local_player_as_initialized、network_stack_latency、tick_sync、player_skin、emote_list、respawn、player_action（respawn / dimension_change_ack / start_item_use_on / stop_item_use_on）、animate、interact mouse_over_entity | bedboo 丢弃它们（计数）：客户端自己发。重生是用户在客户端上点“重生” |
| translated | player_auth_input（move_vector + 标志 → `input`；yaw/pitch → `look`；block_actions start_break/abort_break/stop_break → `button attack`）、mob_equipment（`slot`）、inventory_transaction（item_use click_block/click_air、item_release、item_use_on_entity：先 `look`，再 `button use`/`attack`）、interact open_inventory（`action inventory`）、container_close（按键 ESC）、item_stack_request（`stack`） | bedboo 的路由把它们变成操作 |
| shared | text、command_request | 通过 `send` 发送；后备：T 或 SLASH，然后 `text`，然后 ENTER |
| unsupported | book_edit、block_pick_request、crafting_event、set_default_game_type，以及其他所有 | bedboo 抛出 BeddUnsupportedError（ERR_BEDD_UNSUPPORTED） |

只有在 `stack` 探测不回答 `ok` 时，item_stack_request 才算 unsupported。

## ui 操作

配置项 `ui`（默认 `true`）。`ui` 把游戏界面读成一棵树，就像无障碍 API 暴露桌面程序那样（issue [#21](https://github.com/LiteLDev/BeDD/issues/21)），
并按 `ref` 操作其中的节点。

| 形式 | 回复 |
|---|---|
| `{op:"ui", depth?, scope?, includeInvisible?, maxNodes?}` | `{snapshot, screens, active, roots, nodes{ref: 节点}, truncated}` |
| `{op:"ui", snapshot, action:"click", ref}` | 点击分发后回复 `{ok}` |
| `{op:"ui", snapshot, action:"setText", ref, text}` | `{ok}`：文本框内容变为 `text` |
| `{op:"ui", snapshot, action:"scroll", ref, notches}` | `{ok}` |
| `{op:"oreui", url?, maxNodes?, maxText?, timeoutMs?, force?}` | `{url, title, width, height, nodes[], truncated, findings}`：Ore UI 视图的 DOM |

- 节点是 `{role, name, text, value, state, bounds, boundsNorm, actions, children, source, hash}`：`bounds` 是客户区像素，
  `boundsNorm` 是占客户区的比例，`actions` 是节点接受的动作，`children` 是子节点的 ref，节点一变 `hash` 就变。
- `screens` 是界面栈，`active` 是最上层的界面，`roots` 是它的根节点 ref。`scope` 只读某个 ref 之下的部分；`depth` 和 `maxNodes` 会截断树
  （`truncated:true`）；不可见的控件默认不列出，除非给 `includeInvisible`。
- JSON UI 界面逐个控件读取（SceneStack → `UIScene::mScreenView` → `VisualTree::mRootControl` → `UIControl::mChildren`/`mComponents`）。
  OreUI 界面是一个不透明节点 `{role:"group", source:{kind:"oreui", url}}`。`oreui` 读取其内部：以节点数组给出视图的 DOM（`tag`、`id`、`class`、
  `testId`、`role`、`name`、`text`、`value`、`state`、`bounds`，`children` 为下标）；给出 `url` 时等待 URL 包含它的视图。Ore UI 节点没有 ref：
  用 `mouse` 点击其 `bounds` 的中心。见 [UI 控件树](./ui#ore-ui-界面-oreui)。
- 动作与 input 操作走同一个虚拟鼠标和键盘，所以不需要任何玩家做不到的事。`mouse` 也接受 `ref` + `snapshot` 代替坐标：瞄准节点中心。
- 错误：`stale_ref`（这个 ref 不在该快照里，或者节点已经没了）、`stale_snapshot`（界面变了：重新读树）、`occluded`（有别的控件挡住了节点）、
  `not_ready`（还没有树：游戏在加载）、`unsupported`（节点不支持这个动作）。

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

## input 操作

不受配置项控制。这些操作在游戏线程分发完输入后才回复（`timeoutMs`，默认 5000）。

| op | 字段 | |
|---|---|---|
| `key` | `key`（名字）或 `vk`；`action` press\|down\|up；`holdMs`（60） | 名字：字母、数字、ESC、ENTER、BACK、TAB、SPACE、SHIFT、CTRL、ALT、LEFT/UP/RIGHT/DOWN、HOME、END、PGUP、PGDN、INSERT、DELETE、F1..F24、SLASH、GRAVE、MINUS、EQUALS |
| `text` | `text`；`delayMs` | 每个 UTF-16 单元一个 `WM_CHAR` |
| `mouse` | `action` move\|down\|up\|click\|wheel；`x`,`y`（客户区像素）、`rx`,`ry`（比例）或 `ref` + `snapshot`（ui 节点）；`button` left\|right\|middle；`notches`；`holdMs` | 先移动虚拟光标（给了位置时），再在虚拟 GameInput 鼠标上按按钮/滚轮 |
| `raw` | `action` move\|down\|up\|click\|wheel；`dx`,`dy`（鼠标计数）；`steps`；`button`；`notches`；`holdMs` | 虚拟 GameInput 鼠标：鼠标被锁定时转视角 |
| `release` | | 松开所有按住的键和按钮（不管是谁按的），并结束 `input` 移动覆盖 |
| `focus` | | 重新发送“获得焦点”消息 |
| `window` | `clientWidth`、`clientHeight` | 从不激活窗口；窗口留在屏幕外 |
| `flush` | | 等到所有已投递的输入都分发完 |
| `screenshot` | `format` png\|jpeg；`quality`（jpeg）；`maxWidth`（960）；`region{x,y,w,h}`（客户区像素） | `{width, height, format, data}`，`data` 是 base64 图像。对游戏窗口做 PrintWindow（`PW_RENDERFULLCONTENT`）并用 WIC 编码：窗口被遮挡或在屏幕外时也能截 |

## 错误

| code | 含义 |
|---|---|
| `disabled` | 操作的配置项为 `false`（名字在 `key` 里），或 BeDD 正在卸载 |
| `not_in_level` | 客户端不在世界里（服务端版本：服务器还没启动） |
| `refused` | 游戏拒绝了它（不在 `sendAllow` 里的 id、`releaseOn` 时没有指向方块等） |
| `timeout` | 游戏没有在 `timeoutMs` 内完成（之后可能仍会完成） |
| `bad_args` | 字段缺失或无效，或者游戏读不了的字节 |
| `busy` | 被别的东西占着：有界面打开、更新的 `look`、正在进行的加入、tap 已有订阅方 |
| `stopped` | BeDD 正在卸载 |
| `join_failed` | 客户端放弃了加入，界面上显示原因 |
| `not_allowed` | `loopbackOnly` 不让游戏访问的本机以外主机（不在 `allowHosts` 里） |
| `unavailable` | 操作需要的游戏钩子没装上（看游戏日志） |
| `no_client` | 客户端实例还不存在 |
| `remote` | 世界在本游戏没有主持的服务器上 |
| `server_led` | 该服务器运行 BeDD，由它主导游戏速度 |
| `stale_ref` | `ref` 不在快照里，或者节点已经没了 |
| `stale_snapshot` | 快照之后界面变了 |
| `occluded` | 有别的控件挡住了节点 |
| `not_ready` | 还没有 UI 树 |
| `unsupported` | 节点不支持这个动作 |

没有 `code` 的错误表示游戏根本无法执行这个请求：`unknown op 'x'`（这个版本没有该操作）、消息不是 JSON 对象，
或者游戏线程没有在 `timeoutMs` 内处理输入。

## 多个客户端

可以同时打开多个连接（配置 `wsMaxClients`，默认 8），每个由自己的线程按顺序处理。投递窗口输入的操作在连接之间是串行的，
所以两个客户端的按键或输入的文字不会交错；`ping`、`state`、`tap`、`world`、`look`、`input`、`send`、`stack`、`timescale`、`join` 和读取 `ui`
不加这把锁（它们只读，或把工作交给游戏线程）。一个连接关闭（或其进程退出）时，释放它持有的东西，且不动其他连接持有的：
它的按键和鼠标按钮、它发起的 `button` 按住、它设置的 `input` 覆盖。

对有按客户端状态的单元（`src/BeDD.h`）：`currentClient()` 是当前线程所服务连接的 id（从 1 开始，不复用；游戏线程上为 0）。
用它标记状态，并在 `src/Pipe.cpp` 的 `clientGone()` 里释放（像 `clearMoveInputOf` 和 `releaseGameButtonsOf` 那样），或在 `enable()`
里用 `onPipeClientGone(fn)` 注册回调：两者都在离开的连接线程上运行，在它的按键和按钮释放之后、输入操作串行的情况下（关闭时不运行）。

## 安全

令牌就是边界：能读到发现文件的（当前用户的进程）就能驱动游戏，其他人都不能。服务只监听 `127.0.0.1`，用 `Origin` 规则拒绝浏览器，
并以常数时间比较令牌。

由于每个功能配置项默认都是 `true`，持有令牌能做的事很多：

- `send` 让客户端以登录玩家身份发包，限于 `sendAllow`（默认 text 9 和 command_request 77：聊天和命令，和玩家能输入的一样）。
  id 1（login）、30（inventory_transaction）、144（player_auth_input）和 147（item_stack_request）即使列出也始终被拒绝：它们归客户端所有，
  伪造的包会干扰它的预测和记账。只在你控制的服务器上放宽 `sendAllow`。
- `stack` 能在客户端所在的任何服务器上移动、合成、丢弃和销毁登录玩家的物品。
- tap 包含登录玩家的整个会话。

测试框架用不到时，把 `allowSend`、`allowStack` 或 `tap` 设为 `false`，也不要把令牌给别人。只为你信任的本地页面在 `allowOrigins` 里加来源。

## 服务端目标

用 `--target_type=server` 构建的 BeDD 运行在装了 LeviLamina 的 Bedrock 专用服务器里，提供同样的 WebSocket（发现文件里 `kind:"server"`）
和同样的消息格式。游戏速度由服务端主导：它把自己的速度用 LevelEvent 包（`mEventId` 3007，SimTimeScale；`mPos` 0,0,0；
`mData` 为浮点速度的位，暂停时为 0）广播给每个玩家，速度或暂停改变时发一次，每个玩家生成后再单独发一次；客户端上的 BeDD 跟随它。
其他操作回答 `unknown op`。

| 操作 | 字段 | 回复 |
|---|---|---|
| `ping` | | `protocol`、`pid`、`target:"server"`（没有 `hwnd`） |
| `state` | | `target:"server"`、`clients`、`loopbackOnly`、`stats{loopbackRebinds, serverQueriesRefused}` |
| `server` | | `tick`（世界当前刻）、`tps`（最近 2 秒实测的刻数）、`paused`、`port`（`server-port`）、`players[{name, xuid}]` |
| `timescale` | 无：探测 | `scale`、`paused`、`source` default\|config\|local、`tps{client:null, server}`（实测）、`ticksPerSecond`（速度要求的：20 × scale，暂停时为 0） |
| `timescale` | `scale` 0.05..20 和/或 `paused` true\|false；`timeoutMs`（5000） | 改变后的探测结果。在服务器线程上执行（`ServerInstance::queueForServerThread`，暂停时也会执行）：速度直接写进模拟计时器（`Minecraft::setSimTimeScale` 是乘法且上限为 10），暂停用 `setSimTimePause` |
| `command` | `command`（开头的 `/` 可有可无）；`timeoutMs`（10000） | `success`、`successCount`、`output[]`（翻译后的各行），和服务器控制台执行的一样 |

错误：`not_in_level`（服务器还没启动）、`bad_args`、`timeout`（服务器线程没有回应）。`tps.server` 是服务器实际达到的：
速度很高时，如果一刻的耗时超过速度允许的时间，它会低于 `ticksPerSecond`。
