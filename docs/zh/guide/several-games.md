# 多开

每个游戏实例有自己的控制管道，所以一台测试机可以同时跑好几个游戏。克隆一个实例只要几秒钟、约 90 MB。

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

```powershell
pwsh -NoProfile -File clone-instance.ps1 -Slot 2       # versions\1.26.51.01-hi2，约 90 MB，几秒钟
& .\Start-BeDDGame.ps1 -Instance 1.26.51.01-hi2        # 启动时不把窗口提到前面
Connect-BeDD -PipeName BeDD-1.26.51.01-hi2             # 该实例配置里的 pipeName
```

克隆不存在时，`Start-BeDDGame.ps1 -Slot 2`（或 `-Instance <基础实例>-hi2`）会自己先克隆出来。不给 `-Instance` 时，
它启动 Be.D.D. 找到的基础实例，`-Slot 2` 及以上则启动它的克隆。`clone-instance.ps1` 只接受 1 到 32 号槽位；更大的槽位用
`BeDDSetup.psm1` 的 `New-BeDDClone -Source <基础实例> -Slot 41`。基础实例有变化时，已有的克隆会同步更新。

## 并行测试的规矩

克隆出来的实例有自己的 `mods\`、日志、存档和设置。几条规矩，免得并行的测试互相打架：

- 每个实例用自己的锁文件；按**实例目录**找自己的游戏进程，别只看进程名 `Minecraft.Windows`（不然会关掉别人的游戏）。
- 一定以 `Minecraft.Windows.exe` 这个文件名启动（`Start-BeDDGame.ps1` 就是这样做的）：换个名字，游戏会改用你平时玩的
  Minecraft 数据，而不是这个实例的。
- 测试存档用 `Set-WorldSinglePlayer <存档目录>`（[框架垫片](/zh/guide/harness)里提供）设成单人：同一台电脑上的游戏共用一个联机登录，
  多个多人存档同时打开会报"发生错误"（代码 "Blaze"）。
- 需要在线加入服务器的测试，在 `Start-Game` 之前设 `$script:ExclusiveSignaling = $true`，等其他游戏关掉后独占整台电脑。

## 启动游戏

`Start-BeDDGame.ps1` 直接运行 `versions\<实例>\Minecraft.Windows.exe`，窗口出现时不抢前台（不需要 LeviLauncher：
exe 自己加载 LeviLamina）。它返回游戏进程。

| 参数 | |
|---|---|
| `-Instance <名字>` | 要启动的实例（默认：Be.D.D. 找到的那个） |
| `-Slot N` | 改为启动它的克隆 `<基础实例>-hi<N>` |
| `-VersionsDir <目录>` | LeviLauncher 的 `versions` 目录 |
| `-Label <文字>` | 游戏在管理窗口里的名字（默认：调用脚本所在的项目和 worktree） |
| `-NoManager` | 启动游戏时不启动管理窗口（也可以设 `BEDD_MANAGER=0`） |

## 管理窗口

本次登录后第一次运行 `Start-BeDDGame.ps1` 时会打开**管理窗口**，之后的运行（不管来自哪个终端、测试框架或 agent）都复用它。
每个后台游戏都以实时画面显示在里面，不再有自己的窗口：任务栏上没有按钮，Alt+Tab 里也没有，多个测试同时启动、关闭游戏时
桌面上不会有窗口弹出又消失。它也从不抢焦点。

![看标题栏：只有终端是激活的](/showcase/desktop-poster.webp)

- 每个游戏标注启动它的脚本所在的项目（和 worktree）、实例名、pid，以及是否正在收到输入。`Start-BeDDGame.ps1 -Label` 可以自己起名。
- **关掉管理窗口会关掉里面所有的游戏**（会先确认）。管理窗口被强制结束时游戏也会一起退出，不会在后台留下游戏。
- 双击某个游戏只看它一个。右键可以用自己的鼠标键盘接管它（以及交还给脚本）、打开它的 `latest.log` 或实例目录、或者关掉它。
- 脚本里：`BeDDManager.ps1 -List` 列出游戏，`-Game <pid> -Action show|hide|close` 做右键菜单的事，
  `-Shutdown` 不经确认关掉管理窗口和所有游戏。

需要 PowerShell 7（`pwsh`）。`Start-BeDDGame.ps1 -NoManager`（或 `BEDD_MANAGER=0`）启动游戏时不启动管理窗口；
`Install-BeDDMod -NoManager`（配置 `"manager": false`）让游戏不进已经开着的管理窗口，比如要录制真实屏幕的时候。
