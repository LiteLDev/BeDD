# 世界在哪里运行

Be.D.D. 用三种世界模式之一调试客户端模组。这里说的是世界在哪里运行，不是测试怎么驱动游戏（那是[操作模式](/zh/guide/operation-modes)）。

| 世界模式 | 运行什么 | 适合 |
|---|---|---|
| **仅客户端**（`ClientOnly`，默认） | 装有 BeDD 模组的游戏，在单人或局域网世界里 | 大多数客户端模组测试 |
| **客户端 + 服务端**（`ClientServer`） | 装有 BeDD 的游戏，加上同一台电脑上装有 BeDD 服务端模组的基岩版专用服务器（BDS） | 针对真实服务器测试客户端行为，两边都能脚本控制；服务器的游戏速度带动客户端 |
| **客户端 → 远程**（`Remote`） | 装有 BeDD 的游戏，加入别处的服务器 | 不归你管的服务器（只有客户端装 BeDD） |

每次运行用 `BeDD.exe launch --world-mode <模式>` 选择，或者用 `BeDD.exe mod config <实例> --set mode=<模式>` 给某个克隆固定下来。
三种模式下客户端一侧都一样：[驱动游戏](/zh/guide/usage)、[多开](/zh/guide/several-games)。本页讲服务器。

## 客户端 + 服务端

```powershell
BeDD.exe server init s1                     # %LOCALAPPDATA%\BeDD\servers\s1，准备就绪
BeDD.exe server start s1 --json             # 在后台启动：pid、port、url、token
BeDD.exe launch --slot 1 --world-mode ClientServer --detach
# ……在游戏里加入 127.0.0.1:<port>（join 操作），驱动两边……
BeDD.exe server stop s1                     # 保存世界后退出
```

服务器有自己的 WebSocket，找法和游戏的一样（`run\<pid>.json`，其中 `"kind": "server"`；`BeDD.exe ps --json`）。它回应
`ping`、`state`、`server`（刻、每秒刻数、端口、玩家）、`timescale` 和 `command`：

```js
await server('command', { command: 'time set day' });   // 像服务器控制台那样执行；回复输出的各行
await server('timescale', { scale: 3 });                 // 服务器的游戏速度，玩家跟着它走
```

### 准备服务器

`server init` 用 lip（下载到 `%LOCALAPPDATA%\BeDD\tools`）把 LeviLamina 的服务端包装进一个空目录；这个包会带上 Minecraft
1.26.60 的官方 BDS（从 minecraft.net 下载，约 100 MB），并把 BeDD 的服务端模组放进 `mods\BeDD-server`。和游戏的准备一样，
已经做过的步骤会跳过，同一台电脑上一次只做一个准备，而且从不提问，所以测试框架和 CI 可以每次运行前都调用它。

| 参数 | |
|---|---|
| `<名字>` | 服务器，即 `%LOCALAPPDATA%\BeDD\servers` 下的一个目录（`BEDD_SERVERS_DIR`）；默认 `server` |
| `--port`、`--port-v6` | 默认：服务器以前用的端口，否则取 19140/19141、19142/19143……中第一对空闲的，这样服务器既不会撞上游戏的 19132，也不会互相撞 |
| `--transport` | 默认 `nethernet`：从 1.26.60 起游戏只通过 NetherNet 加入服务器（`raknet` 服务器会报 "Terracotta"）。服务器在 TCP `server-port` 上做信令，通过 UDP 承载游戏；`--server-ip 127.0.0.1`（默认）时它为此公布 `127.0.0.1:<port>`（`server-udp-ports`），因为 BeDD 把两边的套接字都留在回环地址上。第一次加入某个服务器时会问"信任此服务器吗？" |
| `--server-ip` | 默认 `127.0.0.1`：只有本机上的游戏能加入；`''` 在所有网卡上监听 |
| `--online-mode` | 游戏必须用 Xbox 登录才能加入（默认关闭） |
| `--property key=value` | 任意 `server.properties` 键，可重复：`level-name`、`gamemode`（默认 `creative`）、`difficulty`（`peaceful`）、`level-seed`、`max-players`、`allow-cheats`（`true`）、`view-distance` 等 |
| `--set key=value` | 服务端模组 `config\config.json` 的键，可重复：`timeScale`（启动时的游戏速度）、`trace` 等 |

`BeDD.exe server list` 显示每台服务器的目录、端口以及是否在运行；`server remove <名字>` 删除一台。

### 启动和停止

`server start <名字>` 启动 `bedrock_server_mod.exe`（带 LeviLamina 的 BDS），完全没有窗口：桌面上什么都不会出现，也不抢焦点。
服务器输出的一切都写进它目录下的 `logs\console.log`。它一直等到服务器启动好、WebSocket 开始监听，然后输出 `pid`、`port`、
`portV6`、`url`、`token`、`dir` 和 `log`。已经在运行的服务器按原样报告。`--timeout <秒>` 是等待启动的时间（180）。

`server stop <名字>` 发送 `stop`（服务器保存世界后退出），30 秒后还没退出就强制结束。

### 不弹防火墙提示

BDS 在所有网卡上监听，而程序第一次这样做时，Windows 防火墙会在桌面上弹窗问是否允许，每个服务器目录问一次，这会卡住无人值守的运行。
BeDD 的服务端模组改为把服务器的每个套接字都绑定到回环地址（配置 `loopbackOnly`，默认开启，和客户端一样），所以 Windows 从不询问，
也只有本机上的游戏能加入。`server init` 还会设置 `server-ip=127.0.0.1` 并关闭局域网可见，否则它也会绑定基岩版的默认端口。

所以 `server start` 拒绝启动这样的服务器：`mods\BeDD-server` 里没有 BeDD 服务端模组、那里放的是别的模组、或者它的 `config.json`
里有 `"loopbackOnly": false`，并说明是哪种情况。万一 LeviLamina 仍然没能加载模组，它会在服务器打开网络之前把服务器停掉。
`--allow-firewall-prompt` 照样启动，用于需要其他电脑上的游戏加入的服务器（配合 `--server-ip ''`）：这时 Windows 询问时请允许。

### 多台服务器

每台服务器有自己的目录、端口和 WebSocket，所以并行的测试各自启动各自的：给每台起个自己的名字（比如它对应游戏的槽位），不指定 `--port`。

## 客户端 → 远程

```powershell
BeDD.exe mod config 1.26.60.05-1 --set 'allowHosts=["play.example.net"]'
BeDD.exe launch --slot 1 --world-mode Remote --exclusive-signaling --detach
```

除了 `allowHosts` 里的主机，游戏的一切都留在回环地址上（[配置](/zh/guide/configuration#不弹防火墙提示)）。需要联机登录的服务器，
在游戏期间还需要独占整台电脑：`--exclusive-signaling`（[并行测试的规矩](/zh/guide/several-games#并行测试的规矩)）。
