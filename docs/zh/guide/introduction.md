# 简介

Be.D.D. 让 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组的端到端测试**在后台跑**。
脚本在 Minecraft 基岩版里移动玩家、打开菜单、按键、点击，你的鼠标、键盘和窗口照常归你用，还能同时开好几个游戏一起测。

![你的输入只到你自己的应用；脚本通过各自的 WebSocket 驱动三个后台游戏](/showcase/how-it-feels.svg)

::: warning 这是测试工具
`mods\` 目录里有 Be.D.D. 的游戏不理会你真实的鼠标和键盘，而且只显示在管理窗口里。Be.D.D. 只把自己装进它自己的游戏实例克隆，
从不装进你平时玩的那个实例。
:::

## 包含什么

Be.D.D. 是一套测试工具，不是给玩家装的模组。一个发行版（`bedd-windows-x64.zip`）包含：

| 部分 | |
|---|---|
| `BeDD.exe` | 一个程序包办一切：双击打开[管理窗口](/zh/guide/several-games#管理窗口)，带参数运行就是命令行，负责克隆、准备和启动游戏（[驱动游戏](/zh/guide/usage)） |
| `mod\BeDD\` | 客户端 LeviLamina 模组，由 `BeDD.exe mod install` 放进游戏实例 |
| `mod\BeDD-server\` | 同样的东西，给基岩版专用服务器用，用于[客户端 + 服务端测试](/zh/guide/modes) |
| `spec\bedd.asyncapi.yaml` | WebSocket 协议的 AsyncAPI 描述，可以拿来生成客户端（[WebSocket 协议](/zh/reference/websocket)） |

测试通过 `127.0.0.1` 上的 **WebSocket** 和每个游戏通信，什么语言都行：Node、Python、C#、PowerShell，或者
[bedboo](https://github.com/LiteLDev/bedboo) 的 mineflayer API。

只有测试机上的游戏需要装，服务器和玩家都不用。Be.D.D. 不是 lip 包，也不在 LeviMC 模组里上架：从
[Releases](https://github.com/LiteLDev/BeDD/releases) 下载。

## 功能

- **测试时照常用电脑。** 游戏不抢前台、不锁鼠标、不吃你的按键，你点哪儿都不会点进游戏里。
- **三种驱动方式，省的优先。** 给 [bedboo](https://github.com/LiteLDev/bedboo) 用的游戏层操作（走路、转视角、挖、用、聊天），
  可以按名字点击的菜单 UI 树，以及作为最后手段的模拟键盘、鼠标和截图（[操作模式](/zh/guide/operation-modes)）。
- **任何语言。** 本地 WebSocket 上一个请求一条 JSON 消息，有 AsyncAPI 规范描述。
- **多开一起测。** 每个游戏有自己的 WebSocket；克隆一个实例只要几秒钟。
- **一个窗口看全部。** 电脑上所有测试的游戏都实时显示在同一个管理窗口里，不会在桌面上弹来弹去；关掉它就关掉全部游戏。
- **跟真人玩一样。** 按键、聊天、键位绑定、菜单、悬停提示、点击、转视角都和真鼠标键盘一样生效。
- **结果确定。** 每个请求都等游戏处理完才返回，不用靠猜的 `sleep`。
- **不动你的实例。** Be.D.D. 只读取 LeviLauncher 的实例，从不往里写东西。

## 系统要求

- Windows 10 或 11，x64。
- [LeviLauncher](https://docs.levimc.org/LeviLauncher/) **1.1** 或更新版本，装有 Minecraft 基岩版 **1.26.60**
  （GDK 正式版）实例，实例里有 LeviLamina **26.60.x**。

其他都不需要：客户端不需要 PowerShell、lip 或 .NET 运行时。电脑上还什么都没有时，见[在新电脑上准备](/zh/guide/new-pc)。

## 名字

Be.D.D.（Bedrock Deep Dive）得名自《绝区零》的 H.D.D.：绳匠在远处引导代理人穿行空洞，你的脚本在后台引导 Minecraft。

## 下一步

[安装](/zh/guide/installation)，然后[驱动游戏](/zh/guide/usage)。
