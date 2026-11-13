---
layout: home

hero:
  name: Be.D.D.
  text: 在后台跑的 Minecraft 测试
  tagline: 脚本在 Minecraft 基岩版里移动玩家、打开菜单、按键、点击，你的鼠标、键盘和窗口照常归你用，还能同时开好几个游戏。
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
  - title: 三种驱动方式，省的优先
    details: 给 bedboo 的 mineflayer API 用的游戏层操作，按名字点击的菜单 UI 树，模拟键盘、鼠标和截图只作为最后手段。
    link: /zh/guide/operation-modes
  - title: 多开一起测
    details: 每个游戏有自己的 WebSocket；克隆一个实例只要几秒钟。
    link: /zh/guide/several-games
  - title: 一个窗口看全部
    details: 电脑上所有测试的游戏都实时显示在同一个管理窗口里，不会在桌面上弹来弹去；关掉它就关掉全部游戏。
    link: /zh/guide/several-games#管理窗口
  - title: 结果确定
    details: 每个请求都等游戏处理完才返回，不用靠猜的 sleep。窗口被挡住也能截图。
    link: /zh/reference/how-it-works
  - title: 任何语言
    details: 本地 WebSocket 上一个请求一条 JSON 消息，有 AsyncAPI 规范描述。一个程序 BeDD.exe 负责克隆、准备和启动游戏。
    link: /zh/guide/integrating
---

![管理窗口里的三个无头 Minecraft 游戏跟着测试脚本动](/showcase/desktop.webp)

Be.D.D.（Bedrock Deep Dive）得名自《绝区零》的 H.D.D.：绳匠在远处引导代理人穿行空洞，你的脚本在后台引导 Minecraft。

它是给 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组做端到端测试的工具，不是给玩家装的模组：
一个提供 WebSocket 的客户端 LeviLamina 模组，加上 `BeDD.exe`：负责克隆、准备和启动游戏，并把它们都显示在一个窗口里的程序。

```powershell
BeDD.exe launch --slot 1 --detach --json   # {"pid":…,"url":"ws://127.0.0.1:53021/v1","token":"…"}
```

```js
const ws = new WebSocket(`${url}?token=${token}`);                       // Node 24；什么语言都行
ws.onopen = () => {
  ws.send(JSON.stringify({ id: 1, op: 'ui' }));                          // 菜单，读成具名节点
  ws.send(JSON.stringify({ id: 2, op: 'input', move: { x: 0, z: 1 } })); // 向前走（在世界里）
};
ws.onmessage = (e) => console.log(e.data);
```

::: tip 从这里开始
先读[简介](/zh/guide/introduction)，然后[安装](/zh/guide/installation)，再[驱动第一个游戏](/zh/guide/usage)。
:::
