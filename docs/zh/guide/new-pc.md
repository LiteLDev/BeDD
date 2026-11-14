# 在新电脑上准备

## 游戏

```powershell
LeviLauncher.exe account login              # once per PC: a Microsoft account that owns Minecraft
LeviLauncher.exe install 1.26.60 --loader   # the GDK release with LeviLamina 26.60.x
LeviLauncher.exe instance list              # check: 1.26.60.xx, loader 26.60.x
```

需要 LeviLauncher 1.1+（第一个带命令行的版本）。没装时，`BeDD.exe` 把最新版下载到 `%LOCALAPPDATA%\BeDD\tools`，
并提示你用它执行上面几步。

## 用哪个实例

`BeDD.exe` 从 `LeviLauncher.exe instance list --json` 里取 1.26.60 的 LeviLamina 实例作为**基础实例**。从不往里写，也从不启动它。

| 覆盖 | |
|---|---|
| `BEDD_LEVILAUNCHER` | 要询问的 `LeviLauncher.exe`（默认：已安装的，否则用 `tools` 里的） |
| `BEDD_INSTANCE` / `--base <name>` | 有多个 1.26.60 实例时指定基础实例 |

## 克隆

每个游戏都在克隆 `<gameVersion>-<slot>` 里运行（如 `1.26.60.05-2`；1 号槽位也是），有自己的 `mods\`、日志、存档和设置；
游戏文件硬链接到基础实例。

```powershell
BeDD.exe instance list --all                # clones and their LeviLauncher bases
BeDD.exe instance show 1.26.60.05-2         # folder, base, game/loader version, mod, hardlinked or copied
BeDD.exe instance clone --slot 2            # make or update
BeDD.exe instance refresh 1.26.60.05-2      # re-link after the base changed
BeDD.exe instance path 1.26.60.05-2         # its folder
BeDD.exe instance remove 1.26.60.05-2       # delete (never the base)
```

`launch --slot N` 在克隆不存在时创建，基础实例变了时刷新。硬链接要求同一个卷：`%LOCALAPPDATA%\LeviLauncher\Instances`
和 `instances` 不在同一个盘时，每个克隆都是完整复制（GB 级，几分钟）；把 `BEDD_INSTANCES_DIR` 设到基础实例所在的卷上。

## 目录

`%LOCALAPPDATA%\BeDD`（`BEDD_HOME` 可移动）：

| | |
|---|---|
| `instances` | 克隆（`BEDD_INSTANCES_DIR`） |
| `run` | 每个运行中的游戏或服务器一个 `<pid>.json`：URL 和令牌（[选哪个游戏](/zh/guide/usage#选哪个游戏)） |
| `locks` | 槽位锁和信令锁 |
| `servers` | 专用服务器（[客户端 + 服务端](/zh/guide/modes)；`BEDD_SERVERS_DIR`） |
| `tools` | 下载的 LeviLauncher、lip |
| `logs` | 程序日志（`manager.log` 等） |
