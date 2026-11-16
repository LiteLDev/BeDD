---
layout: home

hero:
  name: Be.D.D.
  text: 在后台跑的 Minecraft 测试
  tagline: 脚本玩 Minecraft 基岩版，你的鼠标、键盘和窗口照常归你用。
  image:
    src: /logo.png
    alt: Be.D.D.
  actions:
    - theme: brand
      text: 从这里开始
      link: https://levimc.org/tutorials/zh-Hans/automate/bedd/
    - theme: alt
      text: 下载
      link: https://github.com/LiteLDev/BeDD/releases/latest
    - theme: alt
      text: 展示
      link: /zh/gallery

features:
  - title: 测试时照常用电脑
    details: 游戏从不抢前台、光标或你的按键。
    link: /zh/reference/how-it-works
  - title: 三种驱动方式，省的优先
    details: 先用 bedboo 的游戏层操作，再把菜单读成文本，最后才模拟输入。
    link: /zh/guide/operation-modes
  - title: 多开一起测
    details: 每个游戏一条 WebSocket；克隆一个实例只要几秒。
    link: /zh/guide/several-games
  - title: 一个窗口看全部
    details: 电脑上所有游戏实时显示在一个管理窗口里；关掉它就关掉全部。
    link: /zh/guide/several-games#管理窗口
  - title: 结果确定
    details: 每个请求都等游戏处理完才返回。
    link: /zh/reference/how-it-works
  - title: 任何语言
    details: 本地 WebSocket 上的 JSON，附 AsyncAPI 规范。
    link: /zh/guide/integrating
---

![管理窗口里的三个无头 Minecraft 游戏跟着测试脚本动](/showcase/desktop.webp)

## 是什么

Be.D.D. 在后台运行 [LeviLamina](https://levimc.org/LeviLamina/zh-Hans/) 客户端模组的端到端测试：游戏从不抢前台、
光标或按键，还能同时开多个游戏。每个游戏里有一个提供 WebSocket 的客户端模组；`BeDD.exe` 负责克隆、准备和启动游戏。

```powershell
BeDD.exe launch --slot 1 --json   # {"pid":…,"url":"ws://127.0.0.1:53021/v1","token":"…",…}
```

```js
import bedboo from 'bedboo';
const bot = bedboo.createBot({ transport: 'bedd', bedd: { url, token } });
bot.once('spawn', () => bot.setControlState('forward', true));
```

按键、聊天、键位绑定、菜单、悬停提示、点击和视角都和玩家操作一样生效。每个请求处理完才返回。

![你的输入只到你自己的应用；脚本通过各自的 WebSocket 驱动三个后台游戏](/showcase/how-it-feels.svg)

::: warning 这是测试工具
`mods\` 里有 Be.D.D. 的游戏不理会你真实的鼠标和键盘，只显示在管理窗口里。Be.D.D. 只装进它自己的克隆，从不装进你玩的实例。
:::

## 给谁用

LeviLamina 客户端模组的作者，以及任何要对真实 Minecraft 基岩版客户端做测试的人：CI 任务、回归测试、需要真实客户端而不是
协议模拟的机器人。只有测试机需要它。它不是 lip 包，不在 LeviMC 模组上架：从
[Releases](https://github.com/LiteLDev/BeDD/releases) 获取。

第一次用？教程[用 Be.D.D. 驱动游戏客户端](https://levimc.org/tutorials/zh-Hans/automate/bedd/)带你安装、准备电脑、
跑第一个脚本。

## 包含什么

| 发行版内容 | |
|---|---|
| `BeDD.exe` | 双击：[管理窗口](/zh/guide/several-games#管理窗口)；带参数：负责克隆、准备和启动游戏的命令行 |
| `mod\BeDD\` | 由 `BeDD.exe mod install` 放进实例的客户端模组 |
| `mod\BeDD-server\` | 服务端模组，用于[客户端 + 服务端测试](/zh/guide/modes) |
| `spec\bedd.asyncapi.yaml` | [WebSocket 协议](/zh/reference/websocket)，给代码生成器用 |

## 系统要求

- Windows 10 或 11，x64。
- [LeviLauncher](https://levimc.org/LeviLauncher/zh-Hans/) **1.1+**，装有 Minecraft 基岩版 **1.26.60**（GDK 正式版）实例，
  实例里有 LeviLamina **26.60.x**（[安装与克隆](/zh/guide/installs#用哪个实例)）。

不需要 PowerShell、lip 或 .NET 运行时。

## 在生态里的位置

| 项目 | 作用 |
|---|---|
| [LeviLauncher](https://levimc.org/LeviLauncher/zh-Hans/) | 安装游戏和它的 LeviLamina 实例；Be.D.D. 克隆这个实例，从不往里写 |
| [LeviLamina](https://levimc.org/LeviLamina/zh-Hans/) | 在游戏（以及专用服务器）里加载 Be.D.D. 模组；你要测的模组也跑在它上面 |
| [bedboo](https://levimc.org/bedboo/zh-Hans/) | 推荐的客户端：mineflayer 风格的机器人 API，通过 Be.D.D. 的 WebSocket 驱动游戏 |

不用 bedboo 时，任何有 WebSocket 客户端的语言都行（[操作模式](/zh/guide/operation-modes)、
[接入你的测试](/zh/guide/integrating)）。

## 名字

Bedrock Deep Dive，得名自《绝区零》的 H.D.D.：绳匠在远处引导代理人穿行空洞，你的脚本在后台引导 Minecraft。
