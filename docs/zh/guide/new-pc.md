# 在新电脑上准备

除了游戏本身，什么都不用提前手动准备。`Initialize-BeDD.ps1`（`Start-BeDDGame.ps1` 和 `clone-instance.ps1` 也会调用它）
会找到游戏实例，并把电脑上缺的东西补齐，每一步只做一次：

| 缺什么 | 怎么处理 |
|---|---|
| LeviLauncher | 用已安装的；没有就把最新版下载到 `%LOCALAPPDATA%\BeDD\tools` |
| 游戏本身 | 打开 LeviLauncher，脚本等你用拥有 Minecraft 的微软账号登录并装好 1.26.51（这一步脚本做不了；设了 `CI` 环境变量时直接报错并给出这些说明） |
| LeviLamina | 用 [lip](https://docs.levimc.org/lip/) 装进实例；lip 和它需要的 .NET 运行时会自动下载（按用户安装，不需要管理员） |
| 游戏 exe 里 LeviLauncher 的加载器 | 通过 LeviLauncher 启动一次该实例让它加上，然后关掉游戏。没有加载器的话，游戏会用你平时的 Minecraft 数据，而不是实例自己的 |
| 克隆（`-Slot 2` 及以上） | 从基础实例克隆；基础实例之后有变化时会同步更新 |

```powershell
$i = & .\Initialize-BeDD.ps1 -Slot 2    # versions\<基础实例>-hi2 的 Name、Dir、ModsDir、PipeName
```

## 用哪个实例

Be.D.D. 从 `%APPDATA%\levilauncher.exe` 找 LeviLauncher 的数据，遵循它的"数据目录"设置（`base_root`），
并在其中挑 1.26.51 的实例（优先装了 LeviLamina 的）。

要指向别处，设置这些环境变量：

| 变量 | |
|---|---|
| `BEDD_INSTANCE` | 实例名 |
| `BEDD_VERSIONS_DIR` | LeviLauncher 的 `versions` 目录 |
| `BEDD_LAUNCHER_DATA` | LeviLauncher 的数据目录 |
| `BEDD_LEVILAUNCHER` | `LeviLauncher.exe` |
| `BEDD_LIP` | `lip.exe` |

## 在 PowerShell 里

`BeDDSetup.psm1`（框架垫片会导入它）把同样的步骤提供成函数：

- `Initialize-BeDDInstance [-Name n] [-Slot N]` 补齐缺的东西后返回要用的实例（`Name`、`Dir`、`ModsDir`、`PipeName`）。
- `Get-BeDDInstance` 只描述实例，不做任何改动。
- `New-BeDDClone -Source <基础实例> -Slot N` 克隆实例，32 号以上的槽位也行。
- `Get-LeviLauncherData`、`Get-BeDDVersionsDir` 给出它找到的目录。

## 下一步

[驱动游戏](/zh/guide/usage)。
