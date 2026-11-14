# 配置

模组读取 `mods\BeDD\config\config.json`（不存在则用默认值）。只通过 `BeDD.exe` 修改（文件是硬链接）：

```powershell
BeDD.exe mod config 1.26.60.05-2                                        # print
BeDD.exe mod config 1.26.60.05-2 --set clientWidth=1600 --unset timeScale # --unset: back to default
```

`--set`（`mod install` 也支持）：能按 JSON 解析就按 JSON，否则当字符串。`--world-mode` 设置 `mode` 和
`allowHosts`（[世界在哪里运行](/zh/guide/modes)）。`mod install` 和 `launch` 还会设置
`mods\LeviLamina\config\Config.json` 里的 `modules.crashLogger.builtin`（否则 `CrashLogger.exe` 会闪出控制台）。

## 配置项

| 配置项 | 默认 | |
|---|---|---|
| `clientWidth`、`clientHeight` | `0`（不改） | 窗口大小，让点击位置稳定 |
| `wsPort` | `0`（空闲端口） | `127.0.0.1` 上的 WebSocket 端口，见 `run\<pid>.json`；只在同时只跑一个游戏时固定 |
| `wsMaxClients` | `8` | 同时连接数 |
| `allowOrigins` | `[]` | 允许连接的网页来源；其他 `Origin` 请求头得到 403 |
| `manager` | `true` | 显示在[管理窗口](/zh/guide/several-games#管理窗口)里，30 秒没有管理窗口就关闭；`false`：隐藏，从不自行关闭（无头 CI） |
| `loopbackOnly` | `true` | 所有套接字留在回环地址：不弹防火墙提示，没有来自其他主机的"局域网世界"。服务端模组：其 IPv4/IPv6 端口、局域网发现、NetherNet 同样如此；`state` 报告它 |
| `mode` | `ClientOnly` | `ClientOnly`、`ClientServer`、`Remote`（[世界模式](/zh/guide/modes)）；`state` 和 `join` 报告它 |
| `allowHosts` | `[]` | 仍可访问的本机以外主机（Remote 服务器）；列了主机时，发往其他本机以外主机的数据报一律拒绝（`state` 的 `stats.sendsRefused`） |
| `mute` | 忽略 | 始终静音；`false` 只记一条警告 |
| `trace` | `false` | 把每个输入记进 `logs\latest.log` |
| `timeScale` | `1` | 本游戏主持的世界的游戏速度 0.05..20（`timescale` 操作可改）。在 BeDD 服务器后面时以服务器为准；没有 BeDD 的服务器上无效。服务端模组：启动时生效，并发给玩家 |
| `gameHooks` | `true` | bedboo 操作 `world`、`look`、`input`、`slot`、`action`、`button` |
| `tap` | `true` | `tap` 操作（[Tap 协议](/zh/reference/tap)）：玩家的整个会话；关闭 blob 缓存 |
| `tapLoopback` | `true` | 配合 `tap`：也截取单人世界和本机主持的局域网世界 |
| `tapJournalMiB` | `128` | 加入日志上限；超过后晚接入的读取方得到 `complete:false` |
| `allowSend` | `true` | `send` 操作（聊天、命令） |
| `sendAllow` | `[9, 77, 78, 97]` | `send` 可发的数据包 id（text、command_request、command_block_update、book_edit）；login、inventory_transaction、player_auth_input、item_stack_request 始终拒绝 |
| `allowStack` | `true` | `stack` 操作（物品堆请求） |
| `ui` | `true` | `ui` 操作（[UI 树](/zh/reference/ui)） |
| `uiMaxNodes` | `1000` | `format:"json"` 的 `ui` 回复的默认节点上限（请求的 `maxNodes` 取代它） |

## 功能开关

功能开关 `gameHooks`、`tap`、`allowSend`、`allowStack`、`ui` 默认全开（令牌只有你的用户能读）。关闭后其操作回复 `code:"disabled"`；
例如 `allowSend=false` 迫使聊天走聊天界面。风险见[安全](/zh/reference/websocket#安全)。

## 不弹防火墙提示

套接字绑定到回环以外的地址时，Windows 按 exe 路径（即每个克隆）提示一次。`loopbackOnly` 把通配绑定改到回环地址；
配合 `allowHosts` 时，出站 UDP 套接字改为直接 connect 到该主机（不弹提示）。连远程服务器也不要关掉 `loopbackOnly`。

## 环境变量

| 变量 | 读取方 | |
|---|---|---|
| `BEDD_HOME`、`BEDD_INSTANCES_DIR`、`BEDD_SERVERS_DIR` | `BeDD.exe`（模组：`BEDD_HOME`） | [目录](/zh/guide/new-pc#目录) |
| `BEDD_LEVILAUNCHER`、`BEDD_INSTANCE` | `BeDD.exe` | [用哪个实例](/zh/guide/new-pc#用哪个实例) |
| `BEDD_LABEL` | `BeDD.exe`、模组 | 管理窗口里的名字（`launch --label`） |
| `BEDD_RETURN_FOCUS` | 模组 | 游戏启动时抢了前台就把它还回去（由 `launch` 设置） |
| `BEDD_DIR` | 你的测试框架 | 解压好的 Be.D.D.（[安装](/zh/guide/installation#让测试框架自己下载)） |
