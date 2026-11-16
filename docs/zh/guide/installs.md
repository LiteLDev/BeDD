# 安装与克隆

安装 Be.D.D.、在一台电脑上准备好第一个游戏：见教程
[用 Be.D.D. 驱动游戏客户端](https://levimc.org/tutorials/zh-Hans/automate/bedd/)。本页讲之后的事：升级、CI、实例、克隆和目录。

## 升级

运行新版安装程序；它会关掉开着的管理窗口及其游戏。之后每次 `launch` 在克隆缺模组时安装，模组与来源不同时替换（保留
`config.json`）：

- 默认来源：`BeDD.exe` 旁的 `mod\BeDD`，否则是当前目录所在检出的 `bin\BeDD`；
- `mod install <instance> --from <dir>` 之后：该目录（记在 `mods\BeDD\source.pin`）；目录里不再有构建时，`launch`
  发出警告，不动模组。不带 `--from` 的 `mod install` 结束固定。

不替换：`launch --keep-mod`，或该克隆有游戏在运行时。

## 卸载

设置 > 应用 > 已安装的应用 > **BeDD** 删除程序、开始菜单项和 `PATH` 项。要连 `%LOCALAPPDATA%\BeDD`（克隆、存档、日志）
一起删：对每个克隆运行 `BeDD.exe instance remove --slot N --yes`，再删除该目录。

## 让测试框架自己下载

`BEDD_DIR` 指向解压好的一份。CI 里固定版本（`releases/download/v0.4.0/...`）。

```powershell
curl.exe -L -o "$env:TEMP\bedd.zip" https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-windows-x64.zip
mkdir "$env:LOCALAPPDATA\BeDD\app" -Force; tar.exe -xf "$env:TEMP\bedd.zip" -C "$env:LOCALAPPDATA\BeDD\app"
```

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
