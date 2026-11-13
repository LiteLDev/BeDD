# 配置

模组读取克隆里的 `mods\BeDD\config\config.json`。`BeDD.exe mod install` 会写入默认值；改配置项用 `mod config`，
别直接手改克隆里的文件（它的文件可能是和基础实例共用的硬链接）：

```powershell
BeDD.exe mod config 1.26.60.05-2                                      # 显示当前配置
BeDD.exe mod config 1.26.60.05-2 --set clientWidth=1600 --set clientHeight=900
BeDD.exe mod config 1.26.60.05-2 --set 'allowHosts=["play.example.net"]' --set manager=false
```

值能按 JSON 解析时（`true`、`8`、`["a"]`）按 JSON 读，否则当作字符串。没有配置文件时游戏用默认值运行。

`BeDD.exe mod install` 和 `launch` 还会把克隆的 LeviLamina 切换到内置崩溃记录器（`mods\LeviLamina\config\Config.json`
里的 `modules.crashLogger.builtin`）：默认的 `CrashLogger.exe` 是控制台程序，每次启动游戏都会在桌面上闪一下控制台窗口。

## 配置项

| 配置项 | |
|---|---|
| `clientWidth`、`clientHeight` | 游戏窗口大小（0 = 不改），让每次运行的点击位置都一样 |
| `wsPort` | 默认 `0`：WebSocket 在 `127.0.0.1` 上的端口；0 表示挑一个空闲端口，由 `run\<pid>.json` 报告。只有同时只跑一个游戏时才设固定端口 |
| `wsMaxClients` | 默认 `8`：游戏同时服务的 WebSocket 连接数（每条连接关闭时，它按着的键、按钮和 `input` 覆盖都会被松开） |
| `allowOrigins` | 默认 `[]`：允许连接的网页来源（`"http://localhost:5173"`）。带有不在此列的 `Origin` 请求头的请求一律 403，所以你碰巧打开的网页连不上游戏 |
| `manager` | 默认 `true`：游戏显示在[管理窗口](/zh/guide/several-games#管理窗口)里，30 秒内没有管理窗口接管或失去管理窗口时关闭。不用这个窗口的无头 CI 运行设为 `false`：游戏保持隐藏，从不自行关闭 |
| `loopbackOnly` | 默认 `true`：游戏只留在本机，所以每个新克隆都不会弹 Windows 防火墙提示，也不会去查询本机以外的游戏服务器（它们可能作为"局域网世界"排在你的测试世界前面）。BeDD 的服务端模组里它同样默认开启，把服务器留在回环地址上（IPv4/IPv6 端口、局域网发现和 NetherNet），所以不弹提示；`state` 会报告它。这时服务器端口只能通过 `127.0.0.1` 访问 |
| `mode` | 世界在哪里运行：`ClientOnly`（默认）、`ClientServer` 或 `Remote`（[世界模式](#世界模式)）；`launch --world-mode` 只对一次运行生效。`state` 和 `join` 会报告它 |
| `allowHosts` | `loopbackOnly` 仍然允许游戏访问的主机名或地址（Remote 世界模式的服务器）。会查询这些主机，游戏的出站 UDP 套接字可以不弹防火墙提示地连到它们，发往本机以外其他主机的数据报一律拒绝（`state` 的 `stats.sendsRefused`）。本机上的主机不用列 |
| `mute` | 忽略：BeDD 游戏始终静音（FMOD 不打开音频设备）；设为 `false` 只会记一条警告 |
| `trace` | 把到达游戏的每个输入记进 `logs\latest.log`，用于报告 bug |
| `timeScale` | 默认 `1`：本游戏主持的每个世界的游戏速度（0.05..20），进入世界时设置（之后用 `timescale` 操作修改）。在装有 BeDD 的服务器上，服务器的速度为准，客户端跟随；在没有 BeDD 的服务器上不起作用。在 BeDD 的服务端模组里：服务器启动后生效，并发送给它的玩家 |

## 功能开关

这些配置项开关各种[操作模式](/zh/guide/operation-modes)以及其中风险较高的操作。默认全部**开启**：WebSocket 只在 `127.0.0.1`
上监听，并且要求令牌，而令牌只有你自己的用户能从 `%LOCALAPPDATA%\BeDD\run` 读到。配置项为 `false` 的操作回复
`{"ok":false,"code":"disabled","key":"<配置项>"}`。

| 配置项 | |
|---|---|
| `gameHooks` | 默认 `true`：挂钩游戏自己的系统，用于 bedboo 操作 `world`、`look`、`input`（行走、跳跃、潜行和疾跑，与按键绑定无关）、`slot`、`action` 和 `button`（快捷栏，丢弃/背包/视角/暂停，按游戏刻或直到目标方块破坏为止按住攻击/使用/选取） |
| `tap` | 默认 `true`：`tap` 操作把客户端收发的游戏数据包作为二进制 WebSocket 消息推送（[Tap 协议](/zh/reference/tap)），附带加入日志，读取方可以在会话中途接入。它包含已登录玩家的整个会话。开启时客户端会请求服务器不使用它的 blob 缓存，所以每个区块都完整到达 |
| `tapLoopback` | 默认 `true`，仅在开启 `tap` 时生效：也截取本游戏主持的单人世界和局域网世界（数据包重新序列化；局域网访客的流量不包括在内） |
| `tapJournalMiB` | 默认 `128`：tap 加入日志的大小上限；超过后，较晚接入的读取方拿到的不完整（`complete:false`） |
| `allowSend` | 默认 `true`：`send` 操作，bedboo 之类的脚本用它让客户端发送自己序列化好的聊天和命令 |
| `sendAllow` | `send` 可以发送的数据包 id，默认 `[9, 77]`（text、command_request）；login、inventory_transaction、player_auth_input 和 item_stack_request 始终拒绝 |
| `allowStack` | 默认 `true`：`stack` 操作让脚本以已登录玩家的身份让游戏发送物品堆请求（背包移动、合成、箱子移动），bedboo 就是这样用的 |
| `ui` | 默认 `true`：`ui` 操作，即游戏界面的 [UI 树](/zh/guide/operation-modes#ui-树) |
| `uiMaxNodes` | 默认 `1000`：一次 `ui` 回复最多包含的节点数（请求里的 `maxNodes` 只能调低）；有节点被省略时 `truncated` 会说明 |

某次运行不应该做某件事时就关掉对应的项，比如测试要检查聊天必须走聊天界面时设 `allowSend=false`。每个配置项让 WebSocket
客户端能做什么，见[安全](/zh/reference/websocket#安全)。

## 世界模式

BeDD 游戏在每种模式下都是无头运行；模式说的是世界在哪里运行（[世界在哪里运行](/zh/guide/modes)）。

| 模式 | 世界运行在 | 网络 |
|---|---|---|
| `ClientOnly`（默认） | 客户端里：本地世界 | 只用回环地址 |
| `ClientServer` | 本机上装有 BeDD 的专用服务器里；它的游戏速度带动客户端 | 只用回环地址：在 `127.0.0.1` 加入 |
| `Remote` | 没有 BeDD 的服务器里，可能不在本机；客户端改不了它的游戏速度 | 只用回环地址，`allowHosts` 除外 |

`join` 操作在任何模式下都能加入服务器，标题界面或世界里都可以（[驱动游戏](/zh/guide/usage#服务器)）。

### 不弹防火墙提示

BeDD 游戏从不让 Windows 防火墙弹出"允许访问？"：程序（按完整 exe 路径算，所以每个克隆都是新的）第一次把套接字显式绑定到回环以外的地址时，
防火墙才会询问。开启 `loopbackOnly` 后，三种模式下所有通配绑定都改绑到回环地址；WebSocket 也在 `127.0.0.1` 上监听。Remote 模式下
`allowHosts` 只改变一件事：客户端的出站 UDP 套接字（绑定到端口 0 的通配绑定）完全不显式绑定，而是向允许的服务器 connect，
由此隐式绑定（防火墙不会为此询问）；监听套接字（局域网主持、局域网发现）仍留在回环地址上，发往本机以外其他主机的数据报一律拒绝。
不要为了连远程服务器而关掉 `loopbackOnly`：那样每个新克隆都会重新弹提示。你自己启动的专用服务器是另一个程序：没有 BeDD
回环绑定的服务器，每个服务器目录会问一次。

## 环境变量

| 变量 | 读取方 | |
|---|---|---|
| `BEDD_HOME` | `BeDD.exe`、模组 | Be.D.D. 的目录（默认 `%LOCALAPPDATA%\BeDD`）：克隆、`run`、`locks`、`servers`、`tools`、`logs`（[在新电脑上准备](/zh/guide/new-pc#目录)） |
| `BEDD_INSTANCES_DIR` | `BeDD.exe` | 克隆放在哪里；放在基础实例所在的卷上，克隆才是硬链接而不是复制 |
| `BEDD_SERVERS_DIR` | `BeDD.exe` | [客户端 + 服务端测试](/zh/guide/modes)的专用服务器放在哪里（默认 `%LOCALAPPDATA%\BeDD\servers`） |
| `BEDD_LEVILAUNCHER` | `BeDD.exe` | Be.D.D. 读取其实例的 `LeviLauncher.exe` |
| `BEDD_INSTANCE` | `BeDD.exe` | LeviLauncher 有多个 1.26.60 实例时的基础实例（命令行上用 `--base`） |
| `BEDD_LABEL` | 模组 | 游戏在管理窗口里的名字（`launch --label` 会设置它） |
| `BEDD_RETURN_FOCUS` | 模组 | 一个窗口句柄：游戏启动时如果仍然把自己激活了，就把前台还给这个窗口（`launch` 设为原来在前面的窗口） |
| `BEDD_DIR` | 你的测试框架 | Be.D.D. 解压在哪里（[安装](/zh/guide/installation#让测试框架自己下载)） |
