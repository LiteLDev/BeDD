# 驱动游戏

每个游戏在 `127.0.0.1` 上提供一个 WebSocket：每条消息一个 JSON 请求，游戏处理完后回复一条。先用 bedboo，再用 `ui`，
最后才模拟输入（[原因](/zh/guide/operation-modes#用哪一种)）。

## 启动游戏

```powershell
BeDD.exe launch --slot 1 --json
# {"pid":12345,"instance":"1.26.60.05-1","instanceDir":"…","url":"ws://127.0.0.1:53021/v1","token":"9f3c…","label":"…"}
```

`launch` 克隆、安装或更新模组（[用哪个构建](/zh/guide/installation#升级)），在后台启动游戏，打开
[管理窗口](/zh/guide/several-games#管理窗口)，等待 WebSocket，输出后退出；游戏继续运行。实例已在运行：退出码 5。

| 参数 | |
|---|---|
| `--slot N` / `<instance>` / `--dir <folder>` | 克隆 `<base>-<N>`（默认 1）、指定名字的实例，或任意含 `Minecraft.Windows.exe` 的目录 |
| `--label <text>` | 管理窗口里的名字（默认 `BEDD_LABEL`，否则为调用方项目） |
| `--exclusive-signaling` | 等其他游戏关掉；独占整台电脑（[规矩](/zh/guide/several-games#并行测试的规矩)） |
| `--keep-mod` | 保留克隆里的模组 |
| `--no-manager` | 不启动管理窗口 |
| `--timeout` / `--lock-timeout` | 等待 WebSocket（`3m`）/ 信令锁（`2h`） |
| `--detach` | 进程一启动就返回；`url`/`token` 为 `null` |

`BeDD.exe ps` 列出游戏；`BeDD.exe stop <pid>` 保存并关闭一个。

## 选哪个游戏

`BeDD.exe ps --json`（或 `%LOCALAPPDATA%\BeDD\run\<pid>.json`，WebSocket 监听期间存在）给出每个游戏的 `url`、`token` 和
`instance`。按 `instance` 选自己的，从不按进程名。用 `?token=<token>` 或 `Authorization: Bearer <token>` 连接；令牌每次启动都换新。
详见[连接](/zh/reference/websocket#连接)。

## 用 bedboo

```js
import bedboo from 'bedboo';
const bot = bedboo.createBot({ transport: 'bedd', bedd: { url: game.url, token: game.token } });
bot.once('spawn', () => bot.chat('/summon cow'));
```

## 一个小客户端

不用 bedboo 时，任何 WebSocket 客户端都行：发 `{id, op, ...}`，按 `id` 匹配回复，跳过二进制（tap）帧。本指南用到的
Node 24 `call(op, args)` 助手函数：[连接](/zh/reference/websocket#连接)。按模式分的操作：[操作模式](/zh/guide/operation-modes)；
全部字段：[WebSocket 协议](/zh/reference/websocket)。

## 按键名

`key` 接受按键名（`ESC`、`ENTER`、`F1`..`F24` 等：[input 操作](/zh/reference/websocket#input-操作)）或任意 `vk`。聊天：
bedboo `bot.chat('/summon cow')`；否则 `key T`、`text`、`key ENTER`。

## 位置

`mouse` 接受 `x`,`y` 像素、`rx`,`ry` 比例，或 `ui` 的 `ref`（与窗口大小无关）。用坐标时要固定窗口大小（配置
`clientWidth`/`clientHeight` 或 `window`）。鼠标被锁定时 `raw` 转动视角。

## 服务器

```js
await call('join', { host: '127.0.0.1', port: 19140, wait: true });   // join, wait until in the world
(await call('join')).connection;                                      // none | local | remote
```

在标题界面或世界里都能用（先退出世界）；加入出错或 `timeoutMs`（60000）用完时失败。本机以外的服务器需要
[Remote 世界模式](/zh/guide/modes)。

## 多条连接

每个游戏最多 `wsMaxClients`（8）条连接，每条按顺序处理。一条连接关闭时，它的 `key`/`mouse` 按住和无终点的 `button` 按住被松开；
`button` 的 `click`/`holdTicks`/`releaseOn` 和 `input`（直到 `ttlMs`/`ttlTicks`）继续执行，所以一次性调用方可以直接断开。
详见[多个客户端](/zh/reference/websocket#多个客户端)。
