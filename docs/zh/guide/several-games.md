# 多开

每个游戏有自己的克隆和自己的 WebSocket，所以一台测试机可以同时跑好几个游戏。克隆一个实例只要几秒钟、约 90 MB
（它的游戏文件是指向基础实例的硬链接）。

测试还需要本机的专用服务器时，每次运行同样各起一台，名字和端口各不相同：见[世界在哪里运行](/zh/guide/modes)。

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

```powershell
BeDD.exe instance clone --slot 2            # 1.26.60.05-2，约 90 MB，几秒钟
BeDD.exe mod install 1.26.60.05-2           # 把模组和配置装进这个克隆
BeDD.exe launch --slot 2 --detach --json    # 这个游戏的 pid、url、token
BeDD.exe launch --slot 3 --detach --json    # 再开一个（先对 3 号槽位做 clone 和 mod install）
BeDD.exe ps                                 # 两个都在，带实例和标签
```

## 槽位和克隆

**槽位**就是一个数字；它的克隆是 `%LOCALAPPDATA%\BeDD\instances` 下的 `<游戏版本>-<槽位>`，比如 `1.26.60.05-2`。
1 号槽位也是克隆：Be.D.D. 从不直接启动基础实例。给每个并行的测试分配自己的槽位（比如每个 worktree 或 CI 任务一个）；
两个测试用同一个槽位，就会共用一个克隆和它的存档。

克隆不存在时，`launch --slot N` 会先克隆出来；基础实例有变化时，会先同步更新。`BeDD.exe instance clone|refresh|remove`
可以手动做同样的事（[克隆](/zh/guide/new-pc#克隆)）。槽位没有上限。

## 并行测试的规矩

克隆出来的实例有自己的 `mods\`、日志、存档和设置。几条规矩，免得并行的测试互相打架：

- 按**实例**找自己的游戏（`ps --json`，或者它写的 `run\<pid>.json`），别只看进程名 `Minecraft.Windows`（不然会关掉别人的游戏）。
  `BeDD.exe stop <pid>` 只关你指定的那个游戏。
- 用 `BeDD.exe launch` 启动游戏：它从克隆里运行 `Minecraft.Windows.exe`，正是这样游戏才会用克隆的数据，而不是你平时玩的
  Minecraft 数据。
- 恢复测试存档后，马上用 `BeDD.exe world singleplayer <存档目录>` 把它设成单人：同一台电脑上的游戏共用一个联机登录，
  多个多人存档同时打开会报"发生错误"（代码 "Blaze"）。
- 需要在线加入服务器的测试用 `launch --exclusive-signaling`。每个运行中的游戏在运行期间都持有本机的联机信令锁
  （在 `%LOCALAPPDATA%\BeDD\locks` 里），默认是共享的；带 `--exclusive-signaling` 时，新游戏等其他游戏都关掉后独自持有它，
  在它退出之前其他游戏都不会启动。锁由游戏进程自己持有，所以即使启动它的脚本崩溃了，锁也会释放。

## 管理窗口

每个游戏都显示在**管理窗口**里，也只显示在里面：游戏自己的窗口留在屏幕外，任务栏上没有按钮，Alt+Tab 里也没有，
多个测试同时启动、关闭游戏时桌面上不会有窗口弹出又消失。它也从不抢焦点。双击 `BeDD.exe` 打开它；管理窗口没在运行时
`BeDD.exe launch` 也会打开它，之后的运行（不管来自哪个终端、测试框架或 agent）都复用它。

![看标题栏：只有终端是激活的](/showcase/desktop-poster.webp)

- 每个游戏显示为实时画面，标注它的 `--label`（默认：当前目录所在的项目）、实例名、pid，以及是否正在收到输入。
- **关掉管理窗口会关掉里面所有的游戏**（会先确认）。管理窗口被强制结束时游戏也会一起退出，不会在后台留下游戏。
- 双击某个游戏只看它一个。右键可以关掉它、打开它的 `latest.log` 或实例目录，或者复制它的 WebSocket 地址。
- 脚本里：`BeDD.exe ps` 列出游戏，`BeDD.exe stop <pid>` 关掉一个，`BeDD.exe manager status` 查看窗口是否在运行，
  `BeDD.exe manager stop` 不经确认关掉它和所有游戏。

游戏从不脱离管理窗口运行：30 秒内没有管理窗口接管、或者失去管理窗口的游戏会自行关闭（原因写在它的 `latest.log` 里）。
不用这个窗口的无头 CI 运行，把 `manager` 设为 `false`（[配置](/zh/guide/configuration#配置项)）：这时游戏保持隐藏、从不自行关闭，
用 `BeDD.exe stop` 关掉它。
