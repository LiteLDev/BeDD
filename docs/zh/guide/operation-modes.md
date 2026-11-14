# 操作模式

三种模式同时开启，走同一条 [WebSocket](/zh/guide/usage)。`ping` 报告 `defaultMode` 和本版本里每种模式的操作（`modes`）。

| 模式 | 用于 | 操作 |
|---|---|---|
| **bedboo**（默认） | 玩法：移动、战斗、建造、背包、聊天、命令 | `world` `look` `input` `slot` `action` `button` `settings` `send` `stack` `timescale` `join` `tap` |
| **UI 树** | bedboo 覆盖不到的菜单和界面 | `ui`；`oreui`（原始 Ore UI DOM，调试用） |
| **模拟输入** | 其余一切 | `key` `text` `mouse` `raw` `release` `flush` `focus` `window` `screenshot` |

## 用哪一种

**先 bedboo，再 `ui`，最后模拟输入。**

- bedboo 操作用游戏的语言（数字）回答：精确、小，不受窗口大小、界面缩放和键位绑定影响。
- `ui` 视图每个界面几百个字符；按 `ref` 点击控件，不靠像素。
- 一张截图让 agent 花约 500–1800 个图像 token，还要再截一张来确认，而且像素位置会失效。

保持一条连接（或一个小助手函数）开着：每个操作一条 JSON 消息。用 `world` 或 `ui` 检查结果，不用截图。

## bedboo

[bedboo](https://github.com/LiteLDev/bedboo) 是兼容 mineflayer 的基岩版机器人库。用 `transport: 'bedd'` 时，它把
`bot.dig`、`bot.lookAt`、`bot.setControlState`、`bot.chat`、`bot.clickWindow` 等转成这些操作，并从 `tap` 读取数据包
（[配置](/zh/guide/usage#用-bedboo)；覆盖范围：bedboo 的 BeDD 指南）。

不用 bedboo 时直接发操作（[`call`](/zh/guide/usage#一个小客户端)；字段：[bedboo 操作](/zh/reference/websocket#bedboo-操作)）：

```js
await call('look', { yaw: 90, pitch: 30 });
await call('input', { move: { x: 0, z: 1 }, sprint: true, ttlMs: 2000 });
await call('button', { button: 'attack', action: 'down', releaseOn: 'blockChange' });
```

## UI 树

`ui` 把最前面的界面（JSON UI 或 Ore UI）返回成文本；对某行的 `ref` 执行动作。读取方式、字段、角色和错误：
[UI 树](/zh/reference/ui)。

```
screen start_screen
- button "Play" [e1]
- button "Settings" [e2]
- text "v26.60/LeviLamina-26.60.5"
under: toast_screen, debug_screen, cubemap_background_screen
```

```js
await call('ui', { action: 'click', ref: 'e1' });   // reply: changed, target, screen; view when the screen changed
```

ref 在其界面留在栈上期间有效，跨连接通用。动作使用虚拟鼠标和键盘。遇到 `stale_ref` 时重新读 `ui`。

## 模拟输入

字段：[input 操作](/zh/reference/websocket#input-操作)；按键名和位置：[驱动游戏](/zh/guide/usage#按键名)。

```js
await call('key', { key: 'E' });                                  // open the inventory
await call('mouse', { action: 'click', rx: 0.5, ry: 0.474 });      // or { ref } from ui
await call('screenshot', { format: 'jpeg', region: { x: 0, y: 0, w: 480, h: 270 } }); // works while covered
```
