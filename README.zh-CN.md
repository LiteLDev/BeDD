# Be.D.D.

<img src="docs/public/logo.png" alt="" width="128" align="right">

[English](README.md) | 简体中文

[![Release](https://img.shields.io/github/v/release/LiteLDev/BeDD)](https://github.com/LiteLDev/BeDD/releases)
[![License](https://img.shields.io/github/license/LiteLDev/BeDD)](COPYING.LESSER)
[![Docs](https://img.shields.io/badge/%E6%96%87%E6%A1%A3-docs.levimc.org%2FBeDD-blue)](https://docs.levimc.org/BeDD/zh/)

Be.D.D.（Bedrock Deep Dive）得名自《绝区零》的 H.D.D.：绳匠在远处引导代理人穿行空洞，你的脚本在后台引导 Minecraft。

让 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组的端到端测试**在后台跑**。
脚本（或 agent）通过本机 WebSocket 玩 Minecraft 基岩版，你的鼠标、键盘和窗口照常归你用，还能同时开好几个游戏一起测。

Be.D.D. 是一套测试工具，不是给玩家装的模组：一个应用 `BeDD.exe` 负责克隆、启动和显示游戏，外加它在每次运行时放进游戏的客户端
LeviLamina 模块。**文档：<https://docs.levimc.org/BeDD/zh/>**

<p align="center"><img src="docs/public/showcase/desktop.webp" alt="管理窗口里的三个无头 Minecraft 游戏跟着测试脚本动" width="100%"></p>

> [!WARNING]
> 这是**测试工具**。`mods\` 目录里有 Be.D.D. 的游戏不理会你真实的鼠标和键盘，而且只显示在管理窗口里。
> 别把它留在平时玩的游戏里。

## 功能

- **测试时照常用电脑。** 游戏不抢前台、不锁鼠标、不吃你的按键，你点哪儿都不会点进游戏里。
- **对 agent 友好：先讲语义，最后才看像素。** 三种驱动方式同时可用：通过 [bedboo](https://github.com/LiteLDev/bedboo)
  的语义操作（默认：行走、转视角、使用物品、聊天，把世界当数据读）；界面组件树（类似无障碍树的快照，按 ref 点击、输入、滚动）；
  最后才是模拟键盘鼠标和截图。脚本和 agent 只在别无他法时才截图，能省下大量 token。
- **一个应用。** `BeDD.exe` 就是整个客户端：双击打开管理窗口，或在任何脚本里运行 `BeDD.exe <名词> <动词> [--json]`。
  不需要 PowerShell，没有安装脚本，也不用装运行时。
- **任何语言都能用。** 游戏通过本机 WebSocket（`ws://127.0.0.1:<端口>/v1`，带令牌）收发 JSON，协议由
  [AsyncAPI 规范](spec/bedd.asyncapi.yaml)描述。
- **多开一起测。** 每个槽位是你的 LeviLauncher 实例的硬链接克隆，几秒钟就好，各有自己的 WebSocket。
- **一个窗口看全部。** 电脑上所有测试的游戏都实时显示在同一个管理窗口里，不会在桌面上弹来弹去；关掉它就关掉全部游戏。
- **跟真人玩一样。** 按键、聊天、键位绑定、菜单、悬停提示、点击、转视角都和真鼠标键盘一样生效。
- **结果确定。** 每个调用都等游戏处理完才返回，不用靠猜的 `sleep`。
- **窗口被挡住也能截图。** 管理窗口可以躲在你其他窗口后面。

<p align="center"><img src="docs/public/showcase/how-it-feels.svg" alt="你的输入只到你自己的应用；脚本通过各自的连接驱动三个后台游戏" width="100%"></p>

## 安装

从 [Releases](https://github.com/LiteLDev/BeDD/releases) 下载并运行 `bedd-windows-x64-setup.exe`。它只为你自己安装，
不需要管理员权限，装到 `%LOCALAPPDATA%\Programs\BeDD`，把 `BeDD.exe` 加进 `PATH`，并在开始菜单里加上 **BeDD**。
`bedd-windows-x64.msi` 内容相同，用于静默安装（`msiexec /i bedd-windows-x64.msi /qn`）；`bedd-windows-x64.zip` 不用安装，
解压到任意位置即可。装好后有：

| | |
|---|---|
| `BeDD/BeDD.exe` | 应用本身：管理窗口和命令行 |
| `BeDD/mod/BeDD/` | `BeDD.exe` 每次运行时放进游戏实例的客户端模块 |
| `BeDD/mod/BeDD-server/` | Bedrock 专用服务器用的模块（客户端与服务端模式） |
| `BeDD/spec/` | `bedd.asyncapi.yaml`，WebSocket 协议 |

需要 Minecraft 基岩版 **1.26.60**（GDK 版）和 LeviLamina **26.60.x**（客户端），装成
[LeviLauncher](https://github.com/LiteLDev/LeviLauncher) **1.1 或更新版本**的实例。Be.D.D. 不是 lip 包，也不在 LeviMC 模组里上架。
只有测试机上的游戏需要装，服务器和玩家都不用。

### 你的实例还是你的

实例归 LeviLauncher 管；`BeDD.exe` 只读取它（`LeviLauncher.exe instance list --json`），从不往里写。Be.D.D. 运行的每个游戏
（包括 1 号槽位）都是它的克隆，放在 `%LOCALAPPDATA%\BeDD\instances\<游戏版本>-<槽位>`：游戏文件是硬链接（实例在另一个卷上时改为复制），
所以克隆只要几秒、几乎不占空间，并且有自己的 `mods\`、日志、存档和设置。实例有变化时克隆会同步更新。Be.D.D. 的其他东西也都在
`%LOCALAPPDATA%\BeDD` 下（可用 `BEDD_HOME` 挪走）。

```
BeDD.exe instance list              # 克隆；加 --all 还列出它们来源的 LeviLauncher 实例
BeDD.exe instance clone --slot 2    # 或者让 launch 自己克隆
BeDD.exe instance remove 1.26.60.05-2
```

`LeviLauncher.exe` 不在 BeDD 找的位置时，设置 `BEDD_LEVILAUNCHER`。

## 使用方法

```
BeDD.exe launch --slot 2 --json
```

在管理窗口里无头启动 2 号槽位的游戏，不抢焦点，并输出怎么连它：`pid`、`instance`，以及 WebSocket 的 `url` 和 `token`
（和 `%LOCALAPPDATA%\BeDD\run\<pid>.json` 里的一样）。之后用任何能说 WebSocket 的东西和它对话，比如 Node 22 或更新版本（`node try.mjs`）：

```js
import { execFileSync } from 'node:child_process';
const { url, token } = JSON.parse(execFileSync('BeDD.exe', ['launch', '--slot', '2', '--json']));
const ws = new WebSocket(`${url}?token=${token}`), waiting = new Map(); let id = 0;
ws.onmessage = (e) => { const m = JSON.parse(e.data); waiting.get(m.id)?.(m); waiting.delete(m.id); };
const call = (op, args = {}) => new Promise((r) => { waiting.set(++id, r); ws.send(JSON.stringify({ id, op, ...args })); });
await new Promise((r) => { ws.onopen = r; });
console.log(await call('ping'), await call('world')); ws.close(); // 提供哪些模式；是否在世界里、当前界面、玩家
```

每条消息都是一个 JSON 对象：请求 `{id, op, ...}` 得到 `{id, ok:true, ...}` 或 `{id, ok:false, error, code}`。`ping`
按模式列出游戏提供的操作：

| 模式 | 操作 | 用途 |
|---|---|---|
| **bedboo**（默认） | `world`、`look`、`input`、`slot`、`action`、`button`、`settings`、`send`、`stack`、`timescale`、`join`、`tap` | 按语义玩：读取世界、转向、行走、使用物品、聊天和执行命令、移动物品；[bedboo](https://github.com/LiteLDev/bedboo) 替你说这些 |
| **界面组件树** | `ui` | 把屏幕读成无障碍树（角色、名称、文字、值、位置），按 `ref` 点击、设置文字或滚动某个节点 |
| **输入**（最后手段） | `key`、`text`、`mouse`、`raw`、`release`、`flush`、`focus`、`window`、`screenshot` | 玩家双手做的事，以及画面；对 agent 来说很费 token，别的方式做不到时再用 |

每个请求都等游戏处理完才返回。协议见 [`spec/bedd.asyncapi.yaml`](spec/bedd.asyncapi.yaml)（AsyncAPI 3.0）和
[WebSocket 参考](https://docs.levimc.org/BeDD/zh/reference/websocket)。

`BeDD.exe ps` 列出正在运行的游戏（`--json` 带 `url` 和 `token`），`BeDD.exe stop <pid>` 关掉一个，`BeDD.exe help` 列出所有命令。
每个命令都支持 `--json`，stdout 上只输出结果，失败时以非零退出码退出并给出提示。

### 多开

```
BeDD.exe launch --slot 2 --label checkout --json
BeDD.exe launch --slot 3 --label settings --json
BeDD.exe ps
```

每个槽位是自己的克隆，有自己的 WebSocket。几条规矩，免得并行的测试互相打架：

- 按 **pid 或实例目录**（`ps --json`）找自己的游戏，别只看进程名 `Minecraft.Windows`（不然会关掉别人的游戏）。
- 测试存档用 `BeDD.exe world singleplayer <存档目录>` 设成单人：同一台电脑上的游戏共用一个联机登录，
  多个多人存档同时打开会报"发生错误"（代码 "Blaze"）。

### 客户端与服务端

除了单独一个游戏（进单人世界，或连到远程服务器），Be.D.D. 还能在游戏旁边运行一台装了 BeDD 服务端模块的 Bedrock 专用服务器，
两端都通过 WebSocket 用脚本驱动，客户端的游戏速度跟随服务端：

```
BeDD.exe server init s1       # 用 lip 装 BDS 1.26.60 + LeviLamina，端口从 19140 起
BeDD.exe server start s1      # 没有窗口；它的 WebSocket 出现在 ps 里
BeDD.exe server stop s1
```

见[客户端与服务端模式](https://docs.levimc.org/BeDD/zh/guide/modes)。

### 管理窗口

双击 `BeDD.exe`（或者让第一次 `launch` 自动打开），每个游戏都以实时画面显示在里面，也只显示在里面：游戏自己的窗口留在屏幕外，
任务栏上没有按钮，Alt+Tab 里也没有，多个测试同时启动、关闭游戏时桌面上不会有窗口弹出又消失。它也从不抢焦点。

- 每个游戏标注它的 `--label`（或启动它的项目）、实例名、pid，以及是否正在收到输入。
- **关掉管理窗口会关掉里面所有的游戏**（会先确认）。30 秒内没被管理窗口接管、或者失去管理窗口的游戏会自行关闭，不会在后台留下游戏。
- 双击某个游戏只看它一个。右键可以打开它的 `latest.log` 或实例目录，或者关掉它。
- 脚本里：`BeDD.exe manager status`、`BeDD.exe manager stop`。

### 接入测试框架

任何语言写的测试框架都是这几步：

```
BeDD.exe instance clone --slot 2 --json           # 克隆在哪里（它的 mods\ 和存档）
BeDD.exe mod install 1.26.60.05-2                 # 放入 BeDD 模块；用 mod config 改配置
BeDD.exe world singleplayer <克隆里的存档>         # 恢复测试存档之后
BeDD.exe launch --slot 2 --json                   # url + token；通过 WebSocket 驱动
BeDD.exe stop <pid>; BeDD.exe mod uninstall 1.26.60.05-2
```

把你自己的模组部署到克隆的 `mods\` 里；"把其他模组挪开"那一步要跳过 `BeDD`：`mod install` 每次运行都重新放入，`mod uninstall` 再拿走。

配合 [bedboo](https://github.com/LiteLDev/bedboo)，一个测试就是一个 mineflayer 风格的机器人，操作的是真实客户端：
`transport: 'bedd'` 连接游戏的 WebSocket，从 tap 读取游戏数据包，并把机器人的动作变成上面的操作。

完整的测试框架可以参考 [BeetrootSkin](https://github.com/MiracleForest/BeetrootSkin/tree/main/tools/e2e) 和
[AlleviatedBackpacks](https://github.com/MiracleForest/AlleviatedBackpacks/tree/main/tools/e2e)。

## 设置

克隆里的 `mods\BeDD\config\config.json`，由 `BeDD.exe mod install` 写入，用 `BeDD.exe mod config` 修改。
没有这个文件时游戏照样无头运行，只是用默认值。`mod install` 还会把克隆的 LeviLamina 切到内置崩溃记录器（`mods\LeviLamina\config\Config.json` 里的
`modules.crashLogger.builtin`）：默认的 `CrashLogger.exe` 是控制台程序，每次启动游戏都会在桌面上闪一下黑窗口。

```json
{ "clientWidth": 1600, "clientHeight": 900, "wsPort": 0 }
```

| 键 | |
|---|---|
| `clientWidth`、`clientHeight` | 游戏窗口大小（0 表示不改），每次运行点击位置都一样 |
| `wsPort` | 默认 `0`：WebSocket 在 127.0.0.1 上的端口（0 = 任一空闲端口；发现文件和 `launch --json` 会告诉你是哪个） |
| `wsMaxClients` | 默认 `8`：同时允许的连接数（某个连接断开时，它按住的键、鼠标按钮和 `input` 移动覆盖都会松开） |
| `allowOrigins` | 默认 `[]`：允许连接的浏览器来源；带 `Origin` 头的请求除非列在这里，否则一律拒绝 |
| `manager` | 默认 `true`：30 秒内没有管理窗口接管、或失去管理窗口时游戏关闭。`false` 让它隐藏着运行、不需要管理窗口（用于 CI） |
| `loopbackOnly` | 默认 `true`：游戏只在本机通信，新实例不会弹 Windows 防火墙提示，也不会去查询本机以外的游戏服务器（否则它们可能以"局域网世界"出现在测试存档前面）。测试框架自己启动的专用服务器每个服务器目录还是会问一次，点**取消**即可 |
| `mode` | 世界在哪里运行：`ClientOnly`（默认：本地世界）、`ClientServer`（本机上装了 BeDD 的专用服务器；`loopbackOnly` 不变）、`Remote`（没装 BeDD 的服务器，可以在本机以外）。见[调试模式](https://docs.levimc.org/BeDD/zh/guide/configuration#调试模式)。`state` 和 `join` 会报告它 |
| `allowHosts` | `loopbackOnly` 仍然允许游戏访问的主机名或地址（Remote 模式的服务器）。会查询这些主机，游戏的出站 UDP 套接字能连到它们且不弹防火墙提示，发往本机以外其他主机的数据报会被拒绝（`state` 的 `stats.sendsRefused`）。本机上的主机不用写 |
| `mute` | 已忽略：BeDD 游戏总是静音（FMOD 不打开音频设备）；`false` 只会记一条警告 |
| `trace` | 把到达游戏的每个输入写进 `logs\latest.log`，报 bug 时用 |
| `timeScale` | 默认 `1`：本游戏主持的每个世界的游戏速度（0.05..20），进入世界时设置（之后可用 `timescale` 操作修改；在远程服务器上不起作用） |
| `gameHooks` | 默认 `true`：挂钩游戏自身的系统，供 bedboo 操作 `world`、`look`、`input`、`slot`、`action` 和 `button` 使用 |
| `ui` | 默认 `true`：`ui` 操作（界面组件树） |
| `tap` | 默认 `true`：`tap subscribe` 数据流，包含客户端收发的游戏数据包，供 bedboo 使用，并带加入日志，读取方可在会话中途接入。打开时客户端会请服务器不使用它的 blob 缓存，每个区块都完整发送 |
| `tapLoopback` | 默认 `true`，只在 `tap` 打开时生效：单人世界和本游戏开的局域网世界也接到 tap 上（数据包重新序列化；局域网访客的流量不包括在内） |
| `tapJournalMiB` | 默认 `128`：tap 加入日志的大小上限；超过后中途接入不完整（`complete:false`） |
| `allowSend` | 默认 `true`：`send` 操作，bedboo 借此让客户端发送它序列化好的聊天和命令 |
| `sendAllow` | `send` 可发送的数据包 id，默认 `[9, 77, 78, 97]`（text、command_request、command_block_update、book_edit：玩家在游戏自己的界面里能输入或写下的）；login、inventory_transaction、player_auth_input 和 item_stack_request 始终被拒绝 |
| `allowStack` | 默认 `true`：`stack` 操作，bedboo 借此以登录玩家的身份发出物品堆请求（背包移动、合成、箱子存取） |

令牌就是边界：拿到它的程序能做登录玩家能做的任何事。它只写在 `%LOCALAPPDATA%\BeDD\run\<pid>.json` 里，只有你的用户能读。
把某个功能键设成 `false` 就能拿掉它的操作（之后回复 `code:"disabled"`）。

## 更多

完整文档（含更多视频和截图）：<https://docs.levimc.org/BeDD/zh/>

想自己构建、了解协议或内部原理？见 [CONTRIBUTING.md](CONTRIBUTING.md#development)。

## 许可证

[LGPL-3.0-or-later](COPYING.LESSER) © LeviMC。GameInput 接口布局参考微软公开的 `GameInput.h`（v2），不包含任何微软代码。
