# 常见问题

## 连接时报 401 或 403

- **401**：令牌缺失或错误。`url` 和 `token` 要取自同一个 `run\<pid>.json`（或 `BeDD.exe ps --json`）：游戏每次启动都会换新令牌，
  从上一次运行存下来的令牌不能再用。
- **403**：请求带了 `Origin` 请求头，浏览器和部分 WebSocket 库会自动加上。去掉它，或者把这个来源列进配置的 `allowOrigins`。
- **连接被拒绝**：游戏还没启动好，或者已经退出。`launch --detach` 只在 WebSocket 开始监听后才返回；否则等 `run\<pid>.json` 出现。
- **连接太多**：游戏同时只服务 `wsMaxClients`（8）条连接。关掉不再需要的连接。

## 游戏大约 30 秒后自己关掉了

30 秒内没有管理窗口接管它（或者它失去了管理窗口），所以它关闭了；原因写在它的 `logs\latest.log` 里。没有管理窗口在运行时
`BeDD.exe launch` 会打开一个：用 `BeDD.exe manager status` 检查，再看 `%LOCALAPPDATA%\BeDD\logs\manager.log`。不用这个窗口的无头
CI 运行，把 `manager` 设为 `false`（`BeDD.exe mod config <实例> --set manager=false`）：这时游戏保持隐藏，从不自行关闭。

## 关掉管理窗口，所有游戏都关了

它就是这么设计的（会先确认）：电脑上所有测试的游戏都在里面，游戏也从不脱离它运行。

## 克隆要好几分钟、好几 GB

克隆和基础实例不在同一个卷上，所以文件是复制的，不是硬链接。把 `BEDD_INSTANCES_DIR` 设到基础实例所在盘上的目录（基础实例在
`%LOCALAPPDATA%\LeviLauncher\Instances` 下），然后 `BeDD.exe instance remove` 那个复制出来的克隆，重新克隆。见[克隆](/zh/guide/new-pc#克隆)。

## BeDD.exe 找不到 LeviLauncher 或实例

- **没有 LeviLauncher，或者版本低于 1.1**：旧版本没有命令行，Be.D.D. 没法询问。更新 LeviLauncher，或者把 `BEDD_LEVILAUNCHER`
  指向 1.1 及以上的 `LeviLauncher.exe`。
- **没有 1.26.60 实例**：`LeviLauncher.exe instance list` 显示它有哪些实例；用 `LeviLauncher.exe install 1.26.60 --loader`
  装一个。有多个时用 `BEDD_INSTANCE` 或 `--base` 指定。
- `BeDD.exe instance list --all` 显示 Be.D.D. 看到了什么。

## LeviLamina 版本不匹配

模组是为 Minecraft **1.26.60** 和 LeviLamina **26.60.x** 构建的。游戏版本或 LeviLamina 不对时，模组不会加载（LeviLamina
会把原因记在克隆的 `logs\latest.log` 里），WebSocket 也永远不会出现。在 LeviLauncher 里更新基础实例（`LeviLauncher.exe loader install`），
然后 `BeDD.exe instance refresh <实例>` 让克隆跟上。

## 多开时报"发生错误"（代码 "Blaze"）

同一台电脑上的游戏共用一个联机登录，多个多人存档同时打开会失败。用 `BeDD.exe world singleplayer <存档目录>` 把测试存档设成单人。
必须在线加入服务器的测试用 `BeDD.exe launch --exclusive-signaling`，等其他游戏关掉后独占整台电脑。见
[并行测试的规矩](/zh/guide/several-games#并行测试的规矩)。

## 游戏用了我平时的 Minecraft 存档和设置

只有从克隆里以 `Minecraft.Windows.exe` 启动、且 exe 里有 LeviLauncher 的加载器时，游戏才会用实例自己的数据。用 `BeDD.exe launch`
启动，并确认基础实例是用 LeviLauncher 1.1 或更新版本安装的（它会加入加载器）。

## 每个新克隆都弹 Windows 防火墙提示

开启 `loopbackOnly`（默认开启）时游戏只留在本机，什么都不会问。你启动的没有 BeDD 服务端模组的专用服务器，每个服务器目录仍会问一次；
点**取消**就行。

## 启动游戏时闪一下控制台窗口

那是 LeviLamina 的 `CrashLogger.exe`。`BeDD.exe mod install` 和 `launch` 会把克隆切换到内置崩溃记录器；用其他方式准备的实例，
在 `mods\LeviLamina\config\Config.json` 里设置 `modules.crashLogger.builtin`。

## 我关掉了别人的游戏

按**实例**找自己的游戏（`BeDD.exe ps --json`），别只看进程名 `Minecraft.Windows`，并且给每个并行测试分配自己的槽位。

## `look` 超时，或暂停后游戏操作没反应

暂停的单人世界不走刻，而 `look`、`slot`、`action` 和 `button` 都在客户端刻上执行。在那里 `action pause` 之后，用 `key ESC` 恢复。

## `ui` 回复 `stale_snapshot`、`stale_ref` 或 `not_ready`

`ref` 所在的快照之后界面变了，或者界面还在构建。重新读一次 `ui`，用新的 `snapshot` 和 `ref`。Ore UI 界面显示为一个不透明节点：
用[模拟输入](/zh/guide/operation-modes#模拟输入)驱动它。

## 某个操作返回 `code:"disabled"`

这个克隆的配置里，该操作的[功能开关](/zh/guide/configuration#功能开关)是 `false`：`gameHooks`、`tap`、`allowSend`、`allowStack`
或 `ui`。这个版本没有的操作会返回 `unknown op`。

## 报告 bug

在 [GitHub](https://github.com/LiteLDev/BeDD/issues/new/choose) 上开 issue，附上：

- Be.D.D.、LeviLamina 和 Minecraft 的版本（`BeDD.exe version`）；
- 脚本发了什么（JSON 请求），游戏实际做了什么；
- `mods\BeDD\config\config.json`（`BeDD.exe mod config <实例>`）；
- `logs\latest.log` 的相关部分，最好开着 `"trace": true`（它会显示哪些输入到达了游戏，以及是模拟的还是真实的）。
