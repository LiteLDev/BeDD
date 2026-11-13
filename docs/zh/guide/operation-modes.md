# 操作模式

Be.D.D. 的游戏有三种驱动方式。三种同时开启，走同一条 [WebSocket](/zh/guide/usage)；区别在于测试要说什么、要花多少代价。

| 模式 | 你说的是 | 适合 | 操作 |
|---|---|---|---|
| **bedboo**（默认） | "走到 x，挖这个方块，说声 hi" | 玩法：移动、战斗、建造、背包、聊天、命令 | `world` `look` `input` `slot` `action` `button` `send` `stack` `timescale` `join` `tap` |
| **UI 树** | "点名叫 Play 的按钮" | 菜单和界面：设置、表单、你的模组自己的界面 | `ui` |
| **模拟输入** | "按 E，在 0.5, 0.47 处点击，截个图" | 另外两种够不到的地方 | `key` `text` `mouse` `raw` `release` `flush` `focus` `window` `screenshot` |

`ping` 会告诉你这个版本里每种模式有哪些操作：

```json
{ "ok": true, "protocol": 2, "defaultMode": "bedboo",
  "modes": { "bedboo": ["world", "look", "input", "slot", "action", "button", "send", "stack", "timescale", "join", "tap"],
             "ui": ["ui"],
             "input": ["key", "text", "mouse", "raw", "release", "flush", "focus", "window", "screenshot"] } }
```

## 用哪一种

**先 bedboo，再 UI 树，最后模拟输入。** 区别不只是方便：

- bedboo 操作用游戏的语言回答（玩家在 12.5, 64, -3；方块在第 4012 刻破坏），测试检查的是数字而不是图片，窗口大小、
  界面缩放或键位绑定变了也不会出问题。
- UI 树是文本：几百个具名节点，而不是上百万像素的图片。测试（或者编写测试的 AI agent）按名字找到 "Play"，按引用点击它。
- 模拟输入是盲操作。要知道点哪儿得先截图，要知道点没点中还得再截一张。对 AI agent 来说，每张截图花的 token 远比一次
  `world` 或 `ui` 回复多，而且从截图里估出来的像素位置可能点偏。只把它用在没有别的办法的地方，能用 `world` 或 `ui`
  检查结果时就别再截图。

## bedboo

[bedboo](https://github.com/LiteLDev/bedboo) 是一个兼容 mineflayer 的基岩版机器人库。用 `transport: 'bedd'` 时它不自己讲协议，
而是驱动 Be.D.D. 托管的真实客户端：把 `bot.dig`、`bot.lookAt`、`bot.setControlState`、`bot.chat`、`bot.clickWindow`
等翻译成这些操作，并从 tap 读取游戏的数据包。

```js
import bedboo from 'bedboo';

const bot = bedboo.createBot({ transport: 'bedd', bedd: { url: game.url, token: game.token } });
bot.once('spawn', async () => {
  bot.chat('/give @s diamond_pickaxe');
  await bot.lookAt(bot.entity.position.offset(0, -1, 2));
  await bot.dig(bot.blockAt(bot.entity.position.offset(0, -1, 1)));
  bot.setControlState('forward', true);
});
```

它的选项和覆盖了哪些 mineflayer API，见 bedboo 的 BeDD 指南。不用 bedboo 时，直接发这些操作：

```js
await call('look', { yaw: 90, pitch: 30 });                           // 度
await call('input', { move: { x: 0, z: 1 }, sprint: true, ttlMs: 2000 });
await call('button', { button: 'attack', action: 'down', releaseOn: 'blockChange' });
console.log((await call('world')).player.eye);                       // { x, y, z }
```

## UI 树

`ui` 把游戏的界面返回成一棵树，类似无障碍树：每个节点有 `role`（`button`、`textbox`、`label`、`toggle`、`slider`、
`list`、`group` 等）、`name`、`text` 或 `value`、`state`（启用、焦点、选中等）、以窗口像素和比例表示的 `bounds`，以及它支持的
`actions`。

```js
const ui = await call('ui');
// { snapshot: 7, active: 'start_screen', roots: ['n1'], nodes: { n1: {…}, n12: { role: 'button', name: 'Play', … } } }
const [ref] = Object.entries(ui.nodes).find(([, n]) => n.role === 'button' && n.name === 'Play');
await call('ui', { snapshot: ui.snapshot, action: 'click', ref });

const form = await call('ui', { scope: 'active' });
const [field] = Object.entries(form.nodes).find(([, n]) => n.role === 'textbox');
await call('ui', { snapshot: form.snapshot, action: 'setText', ref: field, text: 'My world' });
```

- `ref` 属于它所在的 `snapshot`。界面变了之后，动作会回复 `stale_snapshot` 或 `stale_ref`：重新读一次 `ui`。
  `occluded` 表示节点被挡住了；`not_ready` 表示界面还在构建。
- `depth`、`scope`、`includeInvisible` 和 `maxNodes`（配置 `uiMaxNodes`，默认 1000）可以让回复小一些；有节点被省略时
  `truncated` 会说明。
- 动作走的是和模拟输入相同的虚拟鼠标和键盘，所以游戏看到的是真实的点击。
- 这棵树覆盖游戏的 JSON UI 界面（大多数菜单、容器、HUD、你的模组的界面）。Ore UI 界面（较新的、基于网页的那些）目前显示为一个节点
  `{ role: 'group', source: { kind: 'oreui', url } }`：这些界面用模拟输入驱动。

## 模拟输入

游戏收到按键、文字、鼠标移动、点击和滚轮，就像有人坐在电脑前一样，而你自己的鼠标和键盘不受影响。

```js
import fs from 'node:fs';

await call('key', { key: 'E' });                                  // 打开背包
await call('mouse', { action: 'click', rx: 0.5, ry: 0.474 });      // 按窗口比例位置点击
await call('key', { key: 'T' }); await call('text', { text: '/give @s apple 3' }); await call('key', { key: 'ENTER' });
await call('raw', { action: 'move', dx: 200 });                    // 转视角

const shot = await call('screenshot', { format: 'png', maxWidth: 960 });
fs.writeFileSync('shot.png', Buffer.from(shot.data, 'base64'));    // 窗口被挡住也能截
```

- `screenshot` 接受 `format`（`png` 或 `jpeg`，配合 `quality`）、`maxWidth`（默认 960）和 `region`；图片尽量小，只需要屏幕一部分时就指定区域。
- `mouse` 也可以用 `ui` 给的 `ref` 和 `snapshot` 代替坐标：点击落在那个节点的中心。
- 按键名和位置见[驱动游戏](/zh/guide/usage#按键名)。
