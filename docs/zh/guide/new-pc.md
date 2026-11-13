# 在新电脑上准备

Be.D.D. 只需要事先准备一样东西：一个装有 LeviLamina 的 Minecraft 1.26.60 LeviLauncher 实例。其余的都由它自己在自己的目录里完成。

## 游戏

登录和安装游戏是 LeviLauncher 的事（脚本没法替你买 Minecraft）：

```powershell
LeviLauncher.exe account login              # 每台电脑一次：一个拥有 Minecraft 的微软账户
LeviLauncher.exe install 1.26.60 --loader   # GDK 正式版，带 LeviLamina 26.60.x
LeviLauncher.exe instance list              # 检查：1.26.60.xx，加载器 26.60.x
```

LeviLauncher 需要 **1.1** 或更新版本：更早的版本没有命令行。`BeDD.exe` 完全找不到 LeviLauncher 时，会把最新版下载到
`%LOCALAPPDATA%\BeDD\tools`，并提示你用它执行上面两步。

## 用哪个实例

Be.D.D. 向 LeviLauncher 询问实例列表（`LeviLauncher.exe instance list --json`），把装有 LeviLamina 的 1.26.60
实例当作**基础实例**。它只读这个实例：Be.D.D. 从不往 LeviLauncher 的实例里写东西，也从不通过启动器启动它。

```powershell
BeDD.exe instance list --all                # BeDD 的克隆，以及它们来自的 LeviLauncher 实例
BeDD.exe instance show 1.26.60.05-1         # 其中一个：目录、基础实例、游戏与加载器版本、模组
```

要指向别的地方：

| | |
|---|---|
| `BEDD_LEVILAUNCHER` | 要询问的 `LeviLauncher.exe`（默认：已安装的那个，否则用 `tools` 里的） |
| `BEDD_INSTANCE` 或 `--base <名字>` | 有多个 1.26.60 实例时，按名字指定基础实例 |

## 克隆

Be.D.D. 启动的每个游戏都是基础实例的**克隆**，1 号槽位也不例外：`<游戏版本>-<槽位>`，比如 `1.26.60.05-2`。
克隆有自己的 `mods\`、日志、存档和设置，几秒钟就能做好，因为游戏文件是指向基础实例的硬链接。

```powershell
BeDD.exe instance clone --slot 2            # 生成 1.26.60.05-2，或把它同步到最新
BeDD.exe instance refresh 1.26.60.05-2      # 基础实例变了之后（更新、新模组）重新链接
BeDD.exe instance path 1.26.60.05-2         # 它的目录，给脚本用
BeDD.exe instance remove 1.26.60.05-2       # 删除它（从不删基础实例）
```

克隆不存在时，`BeDD.exe launch --slot N` 会先克隆出来；基础实例有变化时，会先同步更新。

::: tip 同一个卷
硬链接只能在同一个卷内使用。基础实例（在 `%LOCALAPPDATA%\LeviLauncher\Instances` 下）和 Be.D.D. 的 `instances`
目录不在同一个盘时，每个克隆都是完整复制（好几 GB，要几分钟而不是几秒）。把 `BEDD_INSTANCES_DIR` 设到基础实例所在卷上的目录就能避免；
`instance show` 会告诉你是哪种。
:::

## 目录

Be.D.D. 保存的一切都在 `%LOCALAPPDATA%\BeDD` 下：

| 目录 | |
|---|---|
| `instances` | 克隆（`BEDD_INSTANCES_DIR` 只移动这一个） |
| `run` | 每个运行中的游戏或服务器一个 `<pid>.json`：它的 WebSocket 地址和令牌（[驱动游戏](/zh/guide/usage#选哪个游戏)） |
| `locks` | 槽位锁和联机信令锁，由运行中的游戏持有 |
| `servers` | [客户端 + 服务端测试](/zh/guide/modes)用的基岩版专用服务器（`BEDD_SERVERS_DIR`） |
| `tools` | Be.D.D. 下载的东西：没装 LeviLauncher 时的 LeviLauncher，服务器用的 lip |
| `logs` | 程序自己的日志（`manager.log` 等） |

`BEDD_HOME` 移动整棵目录树，比如放到 CI 机器上较快的数据盘。

## 下一步

[驱动游戏](/zh/guide/usage)。
