# 安装

## 下载

从 [Releases](https://github.com/LiteLDev/BeDD/releases) 下载 `bedd-windows-x64.zip`，解压到任意位置。里面有：

```
BeDD.exe                    程序本体：管理窗口和命令行
mod\BeDD\                   客户端模组（BeDD.dll、manifest.json）
mod\BeDD-server\            服务端模组，用于客户端 + 服务端测试
spec\bedd.asyncapi.yaml     WebSocket 协议，给代码生成器用
```

`BeDD.exe` 是自包含的：不需要 .NET 运行时、PowerShell 或安装程序。愿意的话把它的目录加进 `PATH`；本文的例子都直接写
`BeDD.exe`。

Be.D.D. 不是 lip 包，也不在 LeviMC 模组里上架。

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
BeDD.exe instance clone --slot 1            # 实例的克隆，比如 1.26.60.05-1
BeDD.exe mod install 1.26.60.05-1           # 把 mod\BeDD 放进去并写好配置
BeDD.exe launch --slot 1 --detach           # 在后台启动：pid、url、token
```

游戏会显示在[管理窗口](/zh/guide/several-games#管理窗口)里，管理窗口没开时 `launch` 会把它打开。然后就可以[驱动它](/zh/guide/usage)了。
你平时玩的基础实例不会被碰：Be.D.D. 只把模组装进它自己的克隆，正常启动的游戏还是原来的样子。

## 让测试框架自己下载

测试框架可以自己下载 Be.D.D.，不必要求某处已经有一份。Windows 10 和 11 自带 `curl` 和 `tar`，所以任何 shell 或语言都能这样做；
已经解压好一份时，用 `BEDD_DIR` 指向它：

```powershell
curl.exe -L -o "$env:TEMP\bedd.zip" https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-windows-x64.zip
mkdir "$env:LOCALAPPDATA\BeDD\app" -Force; tar.exe -xf "$env:TEMP\bedd.zip" -C "$env:LOCALAPPDATA\BeDD\app"
```

CI 里请固定版本（`releases/download/v2.0.0/...`），免得新发行版在运行中途改变行为。

## 自己构建

见[构建与贡献](/zh/reference/building)。

## 下一步

电脑上还没有 LeviLauncher 或游戏实例时，读[在新电脑上准备](/zh/guide/new-pc)。否则继续看[驱动游戏](/zh/guide/usage)。
