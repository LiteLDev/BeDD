# 世界在哪里运行

配置 `mode`；别和[操作模式](/zh/guide/operation-modes)混淆。

| 世界模式 | 运行什么 | 用途 |
|---|---|---|
| `ClientOnly`（默认） | 装有 BeDD 的游戏，单人或局域网世界；只用回环地址 | 大多数客户端模组测试 |
| `ClientServer` | 游戏 + 在 `127.0.0.1` 上装有 BeDD 服务端模组的基岩版专用服务器（BDS） | 两边都由脚本控制；以服务器的速度为准 |
| `Remote` | 游戏加入没有 BeDD 的服务器，经 `allowHosts` 可达；速度不可设 | 不归你管的服务器 |

在 `launch` 前按克隆设置：`BeDD.exe mod config <instance> --world-mode <mode>`（`mod install` 也支持）。`Remote`
需要 `--server-host <host>`，主机不在本机时会加进 `allowHosts`。

## 客户端 + 服务端

```powershell
BeDD.exe server init s1                     # %LOCALAPPDATA%\BeDD\servers\s1
BeDD.exe server start s1 --json             # pid, port, portV6, url, token, dir, log
BeDD.exe mod config 1.26.60.05-1 --world-mode ClientServer
BeDD.exe launch --slot 1 --json             # then: join 127.0.0.1:<port>
BeDD.exe server stop s1                     # sends stop (saves); kills after 30 s
```

服务器的 WebSocket（`ps --json` 里 `kind: "server"`）接受 `ping`、`state`、`server`、`timescale`、`command`
（[服务端目标](/zh/reference/websocket#服务端目标)），例如 `{op:"command", command:"time set day"}`。

### 准备服务器

`server init` 用 lip（下载到 `tools`）安装 LeviLamina 的服务端包（带 BDS 1.26.60，约 100 MB）和 BeDD 的服务端模组
（`mods\BeDD-server`）。幂等，每台电脑同时只做一个，从不提问。

| 参数 | 默认 / 含义 |
|---|---|
| `<name>` | `server`；`servers`（`BEDD_SERVERS_DIR`）下的一个目录 |
| `--port`、`--port-v6` | 以前用的端口，否则从 19140/19141 起第一对空闲的（从不用 19132） |
| `--transport` | `nethernet`（`raknet` 会失败："Terracotta"）：TCP `server-port` 做信令，UDP `127.0.0.1:<port>`（`server-udp-ports`）。第一次加入会问"信任此服务器吗？" |
| `--server-ip` | `127.0.0.1`（只有本机能加入）；`''`：所有网卡 |
| `--online-mode` | 关；开：需要 Xbox 登录 |
| `--property key=value` | 任意 `server.properties` 键，可重复；BeDD 默认 `gamemode=creative`、`difficulty=peaceful`、`allow-cheats=true` |
| `--set key=value` | 服务端模组配置项，可重复：`timeScale`、`trace` 等 |

`server list` 显示目录、端口和状态；`server remove <name>` 删除一个。并行运行时给每台服务器起自己的名字（如其游戏的槽位），
不指定 `--port`。

### 启动

`server start <name>` 无窗口运行 `bedrock_server_mod.exe`（输出：`logs\console.log`），并等待 WebSocket
（`--timeout`，180 秒）。为避免防火墙提示，它拒绝启动这样的服务器：`mods\BeDD-server` 里没有 BeDD 的模组、放着别的模组、
或设了 `"loopbackOnly": false`；模组加载失败时停掉服务器。`server init` 设置 `server-ip=127.0.0.1` 并关闭局域网可见。
`--allow-firewall-prompt` 跳过这些检查（其他电脑加入：`--server-ip ''`）。

## 远程

```powershell
BeDD.exe mod config 1.26.60.05-1 --world-mode Remote --server-host play.example.net
BeDD.exe launch --slot 1 --exclusive-signaling --json
```

回环地址以外只能访问 `allowHosts`（[不弹防火墙提示](/zh/guide/configuration#不弹防火墙提示)）。需要联机登录的服务器要用
`--exclusive-signaling`（[规矩](/zh/guide/several-games#并行测试的规矩)）。
