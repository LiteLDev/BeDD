# 简介

Be.D.D. 让 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组的端到端测试**在后台跑**。
脚本在 Minecraft 基岩版里按键、打字、点击，你的鼠标、键盘和窗口照常归你用，还能同时开好几个游戏一起测。

![你的输入只到你自己的应用；脚本通过各自的管道驱动三个后台游戏](/showcase/how-it-feels.svg)

::: warning 这是测试工具
`mods\` 目录里有 Be.D.D. 的游戏不理会你真实的鼠标和键盘，而且只显示在管理窗口里。别把 Be.D.D. 留在平时玩的游戏里。
:::

## 包含什么

Be.D.D. 是一套测试工具，不是给玩家装的模组。一个发行版（`bedd-client-windows-x64.zip`）包含：

| 部分 | |
|---|---|
| `BeDD.dll`、`manifest.json` | 客户端 LeviLamina 模块，测试框架在每次运行时放进游戏实例 |
| `BeDD.psm1` | PowerShell 客户端：`Connect-BeDD`、`Send-BeDDKey`、`Send-BeDDMouse`、`Save-BeDDScreenshot` 等（[驱动游戏](/zh/guide/usage)） |
| `harness-shim.ps1` | 原地替换 LeviLauncher 测试框架的输入函数（[接入测试框架](/zh/guide/harness)） |
| `Start-BeDDGame.ps1` | 启动游戏实例，不把它提到前面 |
| `BeDDManager.ps1` | 显示所有游戏的管理窗口，游戏只显示在这里（[多开](/zh/guide/several-games#管理窗口)） |
| `clone-instance.ps1` | 为并行测试快速克隆一个实例 |
| `BeDDSetup.psm1`、`Initialize-BeDD.ps1` | 找到游戏实例，并补齐电脑上缺的东西（[在新电脑上准备](/zh/guide/new-pc)） |

只有测试机上的游戏需要装，服务器和玩家都不用。Be.D.D. 不是 lip 包，也不在 LeviMC 模组里上架：从
[Releases](https://github.com/LiteLDev/BeDD/releases) 下载。

## 功能

- **测试时照常用电脑。** 游戏不抢前台、不锁鼠标、不吃你的按键，你点哪儿都不会点进游戏里。
- **多开一起测。** 每个游戏实例一条自己的管道；克隆一个实例只要几秒钟。
- **一个窗口看全部。** 电脑上所有测试的游戏都实时显示在同一个管理窗口里，不会在桌面上弹来弹去；关掉它就关掉全部游戏。
- **跟真人玩一样。** 按键、聊天、键位绑定、菜单、悬停提示、点击、转视角都和真鼠标键盘一样生效。
- **结果确定。** 每个调用都等游戏处理完输入才返回，不用靠猜的 `sleep`。
- **窗口被挡住也能截图。** 管理窗口可以躲在你其他窗口后面。
- **直接接进现有测试框架。** 一个文件替换掉 LeviLauncher 测试框架里常见的 `Click`、`Press-Key`、`Focus-Game`
  等函数，测试场景一行不用改。

## 系统要求

- Windows x64；管理窗口需要 PowerShell 7（`pwsh`）。
- Minecraft 基岩版 **1.26.51**（GDK 版）和 LeviLamina **26.51.x**（客户端），装在用
  [LeviLauncher](https://docs.levimc.org/LeviLauncher/) 安装的游戏实例里。测试时用 `Start-BeDDGame.ps1`
  启动这个实例，不走启动器。

电脑上还什么都没有时，除了游戏本身，脚本会把其余的都准备好：见[在新电脑上准备](/zh/guide/new-pc)。

## 名字

Be.D.D.（Bedrock Deep Dive）得名自《绝区零》的 H.D.D.：绳匠在远处引导代理人穿行空洞，你的脚本在后台引导 Minecraft。

## 下一步

[安装](/zh/guide/installation)，然后[驱动游戏](/zh/guide/usage)。
