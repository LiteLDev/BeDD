# 多开

每个游戏有自己的克隆（约 90 MB，几秒钟）和 WebSocket；需要[服务器](/zh/guide/modes)的运行各自启动一个。

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

```powershell
BeDD.exe launch --slot 2 --json
BeDD.exe launch --slot 3 --json
BeDD.exe ps
```

## 槽位和克隆

N 号槽位就是克隆 `<gameVersion>-<N>`（[克隆](/zh/guide/new-pc#克隆)）；1 号槽位也是，基础实例从不运行。没有上限。
每个并行运行（worktree、CI 任务）用自己的槽位：同一槽位意味着共用一个克隆及其存档。

## 并行测试的规矩

- 按**实例**找自己的游戏（`ps --json`、`run\<pid>.json`），从不按进程名 `Minecraft.Windows`；用 `BeDD.exe stop <pid>` 关闭。
- 只用 `BeDD.exe launch` 启动游戏（否则会用你平时的配置文件数据）。
- 恢复测试存档后运行 `BeDD.exe world singleplayer <worldDir>`：游戏共用一个联机登录，同时打开多个多人存档会报 "Blaze"。
- 在线加入服务器的运行用 `launch --exclusive-signaling`：每个游戏在整个生命期内共享持有信令锁（`locks`）；带此参数的游戏
  等其他游戏都关掉后独占它。锁由游戏进程持有，脚本崩溃不会泄漏它。

## 管理窗口

游戏只显示在管理窗口里：在屏幕外，没有任务栏按钮，不在 Alt+Tab 里；管理窗口从不抢焦点。双击 `BeDD.exe`，或由 `launch`
打开；之后的运行复用它。

![看标题栏：只有终端是激活的](/showcase/desktop-poster.webp)

- 每个游戏：实时画面、`--label`、实例、pid、输入活动。双击：只看它。右键：关闭、打开 `latest.log` 或实例目录、
  复制 WebSocket URL。
- 关掉它（先确认）或强制结束它会关掉所有游戏。
- 命令行：`BeDD.exe ps`、`stop <pid>`、`manager status`、`manager stop`（不确认，全部关闭）。

30 秒内没有管理窗口接管或失去管理窗口的游戏会关闭（原因写在 `latest.log`）。无头 CI：配置
`manager=false`（[配置](/zh/guide/configuration#配置项)）；游戏保持隐藏、从不自行关闭；用 `BeDD.exe stop`。
