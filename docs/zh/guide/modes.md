# 客户端与服务端模式

Be.D.D. 调试客户端模组有三种方式：

| 模式 | 运行什么 | 适合 |
|---|---|---|
| **仅客户端** | 装了 BeDD 模块的游戏，进单人或局域网世界 | 大多数客户端模组测试 |
| **客户端 + 服务端** | 装了 BeDD 的游戏，加上同一台电脑上装了 BeDD 服务端模块的 Bedrock 专用服务器（BDS） | 在真实服务器上测客户端行为，两端都能用脚本驱动；客户端的游戏速度跟随服务端 |
| **客户端连远程** | 装了 BeDD 的游戏，连到别处的服务器 | 不归你管的服务器（BeDD 只在客户端） |

三种模式下客户端的用法都一样：[驱动游戏](/zh/guide/usage)、[多开](/zh/guide/several-games)。本页讲服务端。

## 客户端 + 服务端

`BeDDSetup.psm1` 准备服务器，`Start-BeDDServer.ps1` 在后台运行它，两者都在 `bin\BeDD` 里、和模块放在一起：

```powershell
Import-Module .\BeDDSetup.psm1
Initialize-BeDDServer -Name s1 -ModuleDir ..\BeDD-server    # %LOCALAPPDATA%\BeDD\servers\s1，准备就绪
$s = & .\Start-BeDDServer.ps1 -Name s1                       # 启动完成，BeDD 管道也就绪；$s.Port、$s.PipeName
Connect-BeDD -PipeName $s.PipeName                           # 服务端的控制管道（BeDD.psm1）
# ……启动游戏（Start-BeDDGame.ps1），从游戏里加入 127.0.0.1:$($s.Port)……
& .\Start-BeDDServer.ps1 -Name s1 -Stop                      # 保存世界后退出
```

### 准备服务器

`Initialize-BeDDServer` 用 lip 把 LeviLamina 的服务端包装进一个空文件夹；这个包会连带装上 Minecraft 1.26.60 的官方
BDS（从 minecraft.net 下载，约 100 MB）。和准备游戏一样，已经做好的步骤会跳过，一台电脑上同一时间只跑一个准备流程，
而且从不提问，所以测试框架和 CI 可以每次运行前都调用它。服务器的模组放在 `mods\` 下，所以模块装到 `mods\BeDD-server`（LeviLamina 只从以模组名命名的文件夹加载模组）。

| 参数 | |
|---|---|
| `-Name` | 服务器名，即 `%LOCALAPPDATA%\BeDD\servers` 下的文件夹（`BEDD_SERVERS_DIR`）；默认 `server` |
| `-Port`、`-PortV6` | 默认：这台服务器之前的端口，否则取 19140/19141、19142/19143……中第一对空闲的，不会撞上游戏的 19132，服务器之间也不会撞 |
| `-Transport` | 默认 `raknet`：`Join-BeDDServer` 通过 RakNet 加入。BDS 1.26.51 会记录 "TRANSPORT TYPE ERROR"（说没有 NetherNet 玩家无法连接），忽略即可，实际能连上。对只用回环的服务器用 `nethernet` 加入会卡住 |
| `-ServerIp` | 默认 `127.0.0.1`：只有本机的游戏能加入；`''` 监听所有接口 |
| `-OnlineMode` | 默认 `$false`：游戏加入时不做 Xbox 验证 |
| `-LevelName`、`-Gamemode`、`-Difficulty`、`-LevelSeed`、`-MaxPlayers`、`-AllowCheats` | `server.properties`（默认 `Bedrock level`、`creative`、`peaceful`、随机、10、`$true`） |
| `-Properties @{ 'view-distance' = 16 }` | 其他任意 `server.properties` 键 |
| `-ModuleDir` | BeDD 服务端模块（`bin\BeDD-server`），有变化时复制到 `mods\BeDD-server`。没有它，`Start-BeDDServer.ps1` 不会启动服务器（[不弹防火墙提示](#不弹防火墙提示)） |
| `-PipeName` | 服务端里 BeDD 的控制管道，默认 `BeDD-Server-<Name>` |
| `-TimeScale` | 配置 `timeScale`：服务器启动时的游戏速度 |
| `-Trace`、`-Config @{ … }` | 配置 `trace`，以及模块 `config\config.json` 的其他任意键 |

它返回这台服务器：`Dir`、`Port`、`PortV6`、`PipeName`、`LogPath` 和 `Problems`（没有 BeDD 模块时含 `bedd`）。
`Get-BeDDServer -Name s1` 返回同样的内容，但什么都不改。

### 启动与停止

`Start-BeDDServer.ps1 -Name s1` 启动 `bedrock_server_mod.exe`（带 LeviLamina 的 BDS），完全没有窗口：桌面上什么都不会出现，也不会抢焦点。服务器的全部输出
写到它文件夹里的 `logs\console.log`。脚本等服务器启动完成，装了模块时还等 BeDD 的管道出现，然后返回 `Process`、
`PipeName`（没有模块时为 `$null`）、`Port`、`PortV6`、`Dir` 和 `LogPath`。服务器已经在运行时直接返回它。

| 参数 | |
|---|---|
| `-Name`、`-ServersDir` | 哪台服务器 |
| `-Command '<一行>'` | 向服务器控制台输入一行，例如 `-Command 'time set day'` |
| `-Stop` | 发送 `stop`（服务器保存世界后退出），`-StopTimeoutSeconds`（30）后仍未退出就强制结束 |
| `-TimeoutSeconds` | 等它启动的时长（180） |
| `-AllowFirewallPrompt` | 不带 BeDD 的回环绑定也启动（见下文） |

### 不弹防火墙提示

BDS 会监听所有网络接口，而程序第一次这样做时，Windows 防火墙会在桌面上弹窗询问是否允许（每个服务器文件夹一次），无人值守的
测试就会卡住。BeDD 的服务端模块把服务器的所有套接字改为只绑定回环地址（配置 `loopbackOnly`，默认开启，和客户端一样），
所以 Windows 不会询问，也只有本机的游戏能加入。`Initialize-BeDDServer` 还会设置 `server-ip=127.0.0.1` 并关掉局域网可见性，
否则它会另外绑定 Bedrock 的默认端口。

因此 `Start-BeDDServer.ps1` 拒绝启动 `mods\BeDD-server` 里没有 BeDD 服务端模块、装的是别的模块，或 `config.json` 里
`"loopbackOnly": false` 的服务器，并说明原因。万一 LeviLamina 仍没能加载模块，它会在服务器打开网络之前把服务器停掉。
`-AllowFirewallPrompt` 照样启动它，用于要让别的电脑上的游戏加入的服务器（配合 `-ServerIp ''`）：这时在 Windows 询问时允许即可。

### 多台服务器

每台服务器有自己的文件夹、端口和管道，所以并行的测试各自启动自己的服务器：各用一个自己的 `-Name`（比如它的游戏的槽位），
不要给 `-Port`。
