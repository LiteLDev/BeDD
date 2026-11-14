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
    details: 游戏从不抢前台、光标或你的按键。
    link: /zh/guide/introduction
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

[LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组的端到端测试工具：一个提供 WebSocket 的客户端模组，
加上负责克隆、准备和启动游戏的 `BeDD.exe`。

```powershell
BeDD.exe launch --slot 1 --json   # {"pid":…,"url":"ws://127.0.0.1:53021/v1","token":"…",…}
```

```js
import bedboo from 'bedboo';
const bot = bedboo.createBot({ transport: 'bedd', bedd: { url, token } });
bot.once('spawn', () => bot.setControlState('forward', true));
```

开始：[简介](/zh/guide/introduction)、[安装](/zh/guide/installation)、[驱动游戏](/zh/guide/usage)。
