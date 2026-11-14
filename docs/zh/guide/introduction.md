# 简介

Be.D.D. 在后台运行 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组的端到端测试：游戏从不抢前台、
光标或按键，还能同时开多个游戏。

![你的输入只到你自己的应用；脚本通过各自的 WebSocket 驱动三个后台游戏](/showcase/how-it-feels.svg)

::: warning 这是测试工具
`mods\` 里有 Be.D.D. 的游戏不理会你真实的鼠标和键盘，只显示在管理窗口里。Be.D.D. 只装进它自己的克隆，从不装进你玩的实例。
:::

## 包含什么

| 发行版内容 | |
|---|---|
| `BeDD.exe` | 双击：[管理窗口](/zh/guide/several-games#管理窗口)；带参数：负责克隆、准备和启动游戏的命令行 |
| `mod\BeDD\` | 由 `BeDD.exe mod install` 放进实例的客户端模组 |
| `mod\BeDD-server\` | 服务端模组，用于[客户端 + 服务端测试](/zh/guide/modes) |
| `spec\bedd.asyncapi.yaml` | [WebSocket 协议](/zh/reference/websocket)，给代码生成器用 |

测试通过 `127.0.0.1` 上的 WebSocket 和每个游戏通信：先用 [bedboo](https://github.com/LiteLDev/bedboo)，
也可以用任何语言（[操作模式](/zh/guide/operation-modes)）。只有测试机需要它。不是 lip 包，不在 LeviMC 模组上架：
从 [Releases](https://github.com/LiteLDev/BeDD/releases) 获取。

按键、聊天、键位绑定、菜单、悬停提示、点击和视角都和玩家操作一样生效。每个请求处理完才返回。

## 系统要求

- Windows 10 或 11，x64。
- [LeviLauncher](https://docs.levimc.org/LeviLauncher/) **1.1+**，装有 Minecraft 基岩版 **1.26.60**（GDK 正式版）实例，
  实例里有 LeviLamina **26.60.x**（[在新电脑上准备](/zh/guide/new-pc)）。

不需要 PowerShell、lip 或 .NET 运行时。

## 名字

Bedrock Deep Dive，得名自《绝区零》的 H.D.D.：绳匠在远处引导代理人穿行空洞，你的脚本在后台引导 Minecraft。
