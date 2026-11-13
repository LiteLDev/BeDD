# 驱动游戏

装有 Be.D.D. 的游戏在 `127.0.0.1` 上提供一个 **WebSocket**。测试打开它，每条消息发一个 JSON 请求，收到一个 JSON 回复。
什么语言都行，不需要从 Be.D.D. 导入任何东西。

```powershell
BeDD.exe launch --slot 1 --detach --json
# {"pid":12345,"url":"ws://127.0.0.1:53021/v1","token":"9f3c…","instance":"1.26.60.05-1",…}
```

```js
// Node 24：内置 WebSocket
const ws = new WebSocket(`${url}?token=${token}`);
ws.onopen = () => ws.send(JSON.stringify({ id: 1, op: 'world' }));
ws.onmessage = (e) => console.log(JSON.parse(e.data)); // {"id":1,"ok":true,"inLevel":false,"topScreen":"start_screen",…}
```

每个请求都等游戏处理完才返回（或者回复 `ok:false`）。这些请求都不会移动真实光标、按下真实按键或改变前台窗口。

![指哪儿悬停，指哪儿点：虚拟光标能触发物品提示、点击格子](/showcase/inventory-hover.webp)

## 三种操作模式

Be.D.D. 提供三种驱动游戏的方式，同时全部开启。选第一个能完成任务的：

1. **bedboo**（默认）：游戏层操作（`world`、`look`、`input`、`slot`、`action`、`button`、`send`、`stack`、`join`、
   `timescale`、`tap`），最好通过 [bedboo](https://github.com/LiteLDev/bedboo) 的 mineflayer API 使用。
2. **UI 树**：`ui` 把菜单读成一棵由具名按钮、文本框和标签组成的树；按引用点击。
3. **模拟输入**：`key`、`text`、`mouse`、`raw` 和 `screenshot`，像人坐在键盘前一样。最后手段：它需要坐标和截图。

[操作模式](/zh/guide/operation-modes)逐一说明，附带例子，也解释了为什么要按这个顺序。

## 启动游戏

```powershell
BeDD.exe launch --slot 2 --label "my-mod e2e" --detach
```

`launch` 以 `Minecraft.Windows.exe` 启动该槽位的克隆（不存在时先克隆），不把它提到前面；管理窗口还没开时会顺便打开
[管理窗口](/zh/guide/several-games#管理窗口)。带 `--detach` 时，游戏的 WebSocket 开始监听后就返回，并输出它的 `pid`、`url` 和
`token`；不带时输出同样的内容，然后一直等到游戏退出。克隆里需要有模组（`BeDD.exe mod install <实例>`）。

| 参数 | |
|---|---|
| `--slot N` | 克隆 `<基础实例>-<N>`（默认 1） |
| `--label <文字>` | 游戏在管理窗口里的名字（默认：当前目录所在的项目） |
| `--world-mode <模式>` | `ClientOnly`、`ClientServer` 或 `Remote`：[世界在哪里运行](/zh/guide/modes) |
| `--exclusive-signaling` | 等到没有其他游戏开着，并在运行期间独占整台电脑（[多开](/zh/guide/several-games#并行测试的规矩)） |
| `--detach` | 游戏一就绪就返回 |

`BeDD.exe stop <pid>` 关掉一个游戏（保存后退出）；`BeDD.exe ps` 列出正在运行的游戏。

## 选哪个游戏

每个运行中的游戏在 WebSocket 监听期间写入 `%LOCALAPPDATA%\BeDD\run\<pid>.json`，退出时删掉：

```json
{ "protocol": 2, "kind": "client", "pid": 12345, "url": "ws://127.0.0.1:53021/v1", "token": "9f3c…",
  "instance": "1.26.60.05-2", "instanceDir": "C:\\Users\\me\\AppData\\Local\\BeDD\\instances\\1.26.60.05-2",
  "label": "my-mod e2e", "modVersion": "2.0.0", "startedAt": "2026-11-13T09:30:00Z" }
```

按 `launch` 输出的 pid 读取它，或者用 `BeDD.exe ps --json` 列出全部（游戏已经退出的文件会被忽略）。按**实例**找自己的游戏，
别按进程名找，这样就不会驱动到别人的游戏。

令牌是必须的：作为 `?token=<令牌>` 或 `Authorization: Bearer <令牌>` 请求头发送。令牌错误或缺失会得到 HTTP 401。
来自网页的请求（带 `Origin` 请求头）会得到 403，除非配置的 `allowOrigins` 里列了它。

## 一个小客户端

```js
// e2e.mjs，Node 24
import { execFileSync } from 'node:child_process';

const games = JSON.parse(execFileSync('BeDD.exe', ['ps', '--json'], { encoding: 'utf8' }));
const game = games.find((g) => g.instance === '1.26.60.05-1');

const ws = new WebSocket(`${game.url}?token=${game.token}`);
await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });

let nextId = 1;
const pending = new Map();
ws.onmessage = (e) => {
  if (typeof e.data !== 'string') return;          // 二进制帧是 tap 帧
  const reply = JSON.parse(e.data);
  pending.get(reply.id)?.(reply);
  pending.delete(reply.id);
};
const call = (op, args = {}) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, (r) => (r.ok ? resolve(r) : reject(Object.assign(new Error(r.error), r))));
  ws.send(JSON.stringify({ id, op, ...args }));
});

console.log(await call('ping'));                   // protocol 2，defaultMode "bedboo"，modes {…}
await call('timescale', { scale: 3 });
```

用 Python 写也一样，借助 [`websockets`](https://pypi.org/project/websockets/) 包：

```python
import asyncio, json, subprocess
from websockets.asyncio.client import connect

game = json.loads(subprocess.check_output(["BeDD.exe", "ps", "--json"]))[0]

async def main():
    async with connect(f"{game['url']}?token={game['token']}") as ws:
        async def call(op, **args):
            await ws.send(json.dumps({"id": 1, "op": op, **args}))
            reply = json.loads(await ws.recv())
            if not reply["ok"]:
                raise RuntimeError(reply["error"])
            return reply
        print(await call("world"))

asyncio.run(main())
```

全部操作、字段和回复见 [WebSocket 协议](/zh/reference/websocket)参考，机器可读的版本是 `spec\bedd.asyncapi.yaml`。

## 操作

| 操作 | 模式 | 作用 |
|---|---|---|
| `ping` / `state` | | 游戏是否在线、它的窗口、它提供的模式；`state` 还有收到了多少输入、是否抢过前台 |
| `world` | bedboo | 是否在世界中、当前界面、鼠标是否锁定、是否打开菜单、玩家位置、朝向和快捷栏格子 |
| `look` | bedboo | 把玩家转到指定的偏航角和俯仰角（度） |
| `input` | bedboo | 通过游戏自己的输入系统让玩家行走、跳跃、潜行或疾跑一段时间，与按键绑定无关 |
| `slot` / `action` | bedboo | 选择快捷栏格子；丢弃、全部丢弃、打开背包、切换视角、暂停 |
| `button` | bedboo | 按游戏刻或直到目标方块破坏为止按住攻击/使用/选取 |
| `send` | bedboo | 让玩家发送脚本序列化好的聊天或命令数据包 |
| `stack` | bedboo | 让玩家发送脚本序列化好的物品堆请求；`view:true` 显示客户端看到的背包 |
| `join` | bedboo | 按地址加入服务器，标题界面或世界里都可以 |
| `timescale` | bedboo | 本游戏主持的世界的游戏速度，并给出实测的每秒刻数；`paused` 停止刻 |
| `tap` | bedboo | 客户端收发的数据包，作为这条 WebSocket 上的二进制消息（[Tap 协议](/zh/reference/tap)） |
| `ui` | UI 树 | 把菜单读成节点树；按 `ref` 对某个节点 `click`、`setText`、`scroll` |
| `key` / `text` | 输入 | 按键（`action` down/up 按住或松开，`vk` 指定任意虚拟键码）；输入文字，不产生按键 |
| `mouse` / `raw` | 输入 | 在某个位置移动、点击、滚动虚拟鼠标（`x`,`y` 像素或 `rx`,`ry` 比例）；`raw` 转视角 |
| `screenshot` | 输入 | 游戏画面，PNG 或 JPEG，被挡住也照样截 |
| `release` / `flush` / `focus` / `window` | 输入 | 松开所有按着的键和按钮；等所有输入处理完；重发焦点消息；调整窗口大小 |
| `close` | | 关掉游戏 |

bedboo 和 UI 树的操作可以用[功能开关](/zh/guide/configuration#功能开关)关掉；开关关闭的操作回复 `code:"disabled"`。

## 按键名

字母、数字、`ESC`、`ENTER`、`BACK`、`TAB`、`SPACE`、`SHIFT`、`CTRL`、`ALT`、方向键（`LEFT`、`UP`、`RIGHT`、`DOWN`）、
`HOME`、`END`、`PGUP`、`PGDN`、`INSERT`、`DELETE`、`F1`..`F24`、`SLASH`、`GRAVE`、`MINUS`、`EQUALS`。其他键用 `vk`
传虚拟键码。

## 位置

`mouse` 的坐标在游戏窗口客户区内：`x`,`y` 是像素，`rx`,`ry` 是宽高的比例。固定好窗口大小（配置
`clientWidth`/`clientHeight`，或 `window` 操作），每次运行点击位置才一样。更好的做法是按 `ref` 点击 `ui` 节点：
那样完全不依赖窗口大小。

鼠标按钮和滚轮发给一个虚拟鼠标，游戏读它而不读你的鼠标。`raw` 按原始计数移动这个鼠标，游戏锁定鼠标时
（在世界里、没打开界面）就是转视角。

## 聊天和命令

![text 操作在聊天栏里输入 /summon sheep 和 /summon cow](/showcase/chat-summon.webp)

`text` 以字符形式输入文字，不产生按键，所以先打开聊天栏（`key T` 或 `SLASH`），再用 `key ENTER` 发送。这样输入的命令，
游戏就像玩家亲手输入一样执行。用 bedboo 的话，`bot.chat('/summon cow')` 通过 `send` 做同样的事，不用打开聊天界面。

## 服务器

`join` 让客户端像在"服务器"页"添加服务器"再"开始游戏"那样加入服务器，不用点击：在标题界面，或在世界里（先退出世界）。
开始连接后返回；带 `wait:true` 时进入服务器的世界后返回，加入失败（游戏会显示原因）或 `timeoutMs`（默认 60000）用完时失败。
不带 `host` 时报告客户端在哪里玩：`connection` 为 `none`、`local`（自己的世界）或 `remote`（`host`、`port`），以及
[世界模式](/zh/guide/modes)。

```js
await call('join', { host: '127.0.0.1', port: 19140, wait: true });   // 本机上的专用服务器
(await call('join')).connection;                                      // 'remote'
```

本机以外的服务器需要 Remote 世界模式（`BeDD.exe launch --world-mode Remote`，再加上 `allowHosts`）：否则游戏只能连到本机上的服务器。

## 多条连接

多个脚本可以同时使用同一个游戏（配置 `wsMaxClients`，默认 8）：比如一个 bedboo 机器人、一个 UI 检查器和管理窗口。
每条连接上的请求按顺序处理。连接关闭时，它还按着的键和按钮、它设置的 `input` 都会被松开。
