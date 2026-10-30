---
layout: home

hero:
  name: Be.D.D.
  text: 在后台跑的 Minecraft 测试
  tagline: 脚本在 Minecraft 基岩版里按键、打字、点击，你的鼠标、键盘和窗口照常归你用，还能同时开好几个游戏。
  image:
    src: /logo.png
    alt: Be.D.D.
  actions:
    - theme: brand
      text: 开始使用
      link: /zh/guide/introduction
    - theme: alt
      text: 下载
      link: https://github.com/LiteLDev/BeDD/releases/latest
    - theme: alt
      text: 展示
      link: /zh/gallery

features:
  - title: 测试时照常用电脑
    details: 游戏不抢前台、不锁鼠标、不吃你的按键，你点哪儿都不会点进游戏里。
    link: /zh/guide/introduction
  - title: 多开一起测
    details: 每个游戏实例一条自己的管道；克隆一个实例只要几秒钟。
    link: /zh/guide/several-games
  - title: 一个窗口看全部
    details: 电脑上所有测试的游戏都实时显示在同一个管理窗口里，不会在桌面上弹来弹去；关掉它就关掉全部游戏。
    link: /zh/guide/several-games#管理窗口
  - title: 跟真人玩一样
    details: 按键、聊天、键位绑定、菜单、悬停提示、点击、转视角都和真鼠标键盘一样生效。
    link: /zh/guide/usage
  - title: 结果确定
    details: 每个调用都等游戏处理完输入才返回，不用靠猜的 sleep。窗口被挡住也能截图。
    link: /zh/reference/how-it-works
  - title: 直接接进现有测试框架
    details: 一个文件替换掉 LeviLauncher 测试框架里常见的 Click、Press-Key、Focus-Game 等函数，测试场景一行不用改。
    link: /zh/guide/harness
---

![管理窗口里的三个无头 Minecraft 游戏跟着测试脚本动](/showcase/desktop.webp)

Be.D.D.（Bedrock Deep Dive）得名自《绝区零》的 H.D.D.：绳匠在远处引导代理人穿行空洞，你的脚本在后台引导 Minecraft。

它是给 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组做端到端测试的工具，不是给玩家装的模组：
测试框架在每次运行时放进游戏实例的客户端 LeviLamina 模块、驱动它的 PowerShell 客户端，以及准备、克隆、启动游戏的脚本。

```powershell
Import-Module .\mods\BeDD\BeDD.psm1
Connect-BeDD                               # 等游戏启动好
Send-BeDDKey E                             # 打开背包
Send-BeDDMouse click -RX 0.5 -RY 0.474     # 按窗口比例位置点击
Send-BeDDKey T; Send-BeDDText '/give @s apple 3'; Send-BeDDKey ENTER
Save-BeDDScreenshot C:\temp\shot.png       # 窗口被挡住也能截
```

::: tip 从这里开始
先读[简介](/zh/guide/introduction)，然后[安装](/zh/guide/installation)，再[驱动第一个游戏](/zh/guide/usage)。
:::
