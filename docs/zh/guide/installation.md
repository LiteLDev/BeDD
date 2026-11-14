# 安装

## 安装

从 [Releases](https://github.com/LiteLDev/BeDD/releases) 选一个下载：

| | |
|---|---|
| `bedd-windows-x64-setup.exe` | 安装程序：运行，同意许可，完成 |
| `bedd-windows-x64.msi` | 同上，用于静默安装和统一部署：`msiexec /i bedd-windows-x64.msi /qn` |
| `bedd-windows-x64.zip` | 不用安装：解压到任意位置，直接运行里面的 `BeDD.exe` |

安装程序不需要管理员权限：它只为你自己安装，装到 `%LOCALAPPDATA%\Programs\BeDD`，在开始菜单里加一个 **BeDD**（打开[管理窗口](/zh/guide/several-games#管理窗口)），
并把这个目录加进你的 `PATH`，新开的终端就能找到 `BeDD.exe`。这个目录（以及 zip）里有：

```
BeDD.exe                    程序本体：管理窗口和命令行
mod\BeDD\                   客户端模组（BeDD.dll、manifest.json）
mod\BeDD-server\            服务端模组，用于客户端 + 服务端测试
spec\bedd.asyncapi.yaml     WebSocket 协议，给代码生成器用
```

`BeDD.exe` 是自包含的：不需要 .NET 运行时，也不需要 PowerShell。Be.D.D. 不是 lip 包，也不在 LeviMC 模组里上架。

### 升级

运行新版本的安装程序。管理窗口开着的话，安装程序会先关掉它，它显示的游戏也随之关闭。克隆里的模组在下次启动前保持不变：
`BeDD.exe launch` 发现克隆里的模组与已安装的 `BeDD.exe` 自带的不同时，会换成新的，并保留克隆的 `config.json`。
`launch --keep-mod` 则用克隆现有的模组启动。

### 卸载

在 设置 > 应用 > 已安装的应用 里卸载 **BeDD**。这会删掉程序、开始菜单项和 `PATH` 里的那一项；`%LOCALAPPDATA%\BeDD` 里的克隆、
世界和日志会留下。要把它们也删掉，先逐个删除克隆（`BeDD.exe instance remove --slot N --yes`），再删除这个目录。你的 LeviLauncher
实例始终不会被碰。

## 系统要求

- Windows 10 或 11，x64。
- [LeviLauncher](https://docs.levimc.org/LeviLauncher/) **1.1** 或更新版本（第一个带命令行的版本），装有 Minecraft
  基岩版 **1.26.60**（GDK 正式版）实例，实例里有 LeviLamina **26.60.x**（客户端）。

还没有这个实例的话，用 LeviLauncher 自己的命令行装一个：

```powershell
LeviLauncher.exe account login              # 一个拥有 Minecraft 的微软账户
LeviLauncher.exe install 1.26.60 --loader   # 游戏，带 LeviLamina
```

只有测试机上的游戏需要 Be.D.D.，服务器和玩家都不用。Be.D.D. 怎么找到这个实例，见[在新电脑上准备](/zh/guide/new-pc)。

## 手动试试

```powershell
BeDD.exe launch --slot 1 --detach           # 克隆实例（1.26.60.05-1）、装上模组、启动它
```

克隆或模组不存在时 `launch` 会先做好；`BeDD.exe instance clone --slot 1` 和 `BeDD.exe mod install 1.26.60.05-1` 只做其中一步
（`mod install` 还能写[配置](/zh/guide/configuration)项）。

游戏会显示在[管理窗口](/zh/guide/several-games#管理窗口)里，管理窗口没开时 `launch` 会把它打开。然后就可以[驱动它](/zh/guide/usage)了。
你平时玩的基础实例不会被碰：Be.D.D. 只把模组装进它自己的克隆，正常启动的游戏还是原来的样子。

## 让测试框架自己下载

测试框架可以自己下载 Be.D.D.，不必要求已经安装了一份。Windows 10 和 11 自带 `curl` 和 `tar`，所以任何 shell 或语言都能这样做；
已经解压好一份时，用 `BEDD_DIR` 指向它：

```powershell
curl.exe -L -o "$env:TEMP\bedd.zip" https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-windows-x64.zip
mkdir "$env:LOCALAPPDATA\BeDD\app" -Force; tar.exe -xf "$env:TEMP\bedd.zip" -C "$env:LOCALAPPDATA\BeDD\app"
```

CI 里请固定版本（`releases/download/v0.4.0/...`），免得新发行版在运行中途改变行为。

## 自己构建

见[构建与贡献](/zh/reference/building)。

## 下一步

电脑上还没有 LeviLauncher 或游戏实例时，读[在新电脑上准备](/zh/guide/new-pc)。否则继续看[驱动游戏](/zh/guide/usage)。
