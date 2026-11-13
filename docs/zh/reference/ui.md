# UI 树

`ui` 操作把游戏界面读成一棵控件树（类似无障碍树），并可按引用对控件执行操作。它是驱动游戏的第三种方式，与 bedboo 的游戏操作和普通的键盘鼠标输入并列：脚本先问屏幕上有什么，然后点击“Resume Game 按钮”而不是某个像素，用控件状态做断言而不是比对截图。操作不是捷径：BeDD 把虚拟指针移到控件上，使用与 `mouse`、`key`、`text` 操作相同的鼠标、按键和文字输入。设计见 [issue #21](https://github.com/LiteLDev/BeDD/issues/21)。

配置 `ui`（默认 `true`）控制开关；关闭时 `{op:"ui"}` 和带 `ref` 的 `mouse` 回复 `{ok:false, code:"disabled", key:"ui"}`。`uiMaxNodes`（默认 `1000`）限制一次快照的节点数。

## 快照

```json
{ "op": "ui" }
```

| 字段 | 默认 | |
|---|---|---|
| `scope` | `active` | `active`（最前面的界面）、`all`、界面名或界面 id（`scene-…`） |
| `depth` | 全部 | 每个界面根节点以下的层数 |
| `includeInvisible` | `false` | 也返回隐藏的控件（`state.visible:false`） |
| `compact` | `true` | 只返回能看到、能读或能操作的控件，以及包含多个这类控件的分组；`false` = 全部控件 |
| `maxNodes` | `uiMaxNodes` | 节点上限，超过时 `truncated:true` |
| `timeoutMs` | `3000` | 等待客户端线程的时间，超时回复 `code:"not_ready"` |
| `debug` | `false` | 每个节点附带组件及其虚表、原始可见性位 |

回复的字段与英文页相同：`snapshot`、`screens`（场景栈，自下而上）、`active`、`roots`、`nodes`（`ref` → `role`、`name`、`text`、`value`、`state`、`bounds`、`boundsNorm`、`actions`、`children`、`parent`、`surface`、`source`、`hash`）、`truncated`、`client`。

- `screens[].kind`：`jsonui`（逐个控件读取）、`oreui`（一个不透明节点）、`unknown`（其他场景，如游戏内的 play screen，不读取）。
- `role`：`button`、`toggle`、`text_input`、`slider`、`scroll`、`text`、`item`（集合中接受输入但没有已知按钮组件的格子）或 `group`。
- `name` 是控件在 JSON UI 定义里的名字；`text` 是绘制出来的本地化文字（按钮或开关取其第一个可见标签）；`value` 是文本框的文字、滑块的 0..1、开关是否选中。
- `bounds` 是客户区像素，与 `mouse` 的 `x`/`y` 同一坐标系；`boundsNorm` 是窗口比例，与 `rx`/`ry` 相同。
- `source.path` 是控件路径（同名兄弟用 `name[k]` 区分）；集合中的格子另有 `collectionName` 和 `collectionIndex`（背包格：`inventory_items` / 9）。
- `hash` 覆盖 role、name、text 和 source：其中任何一项变化，引用即失效。
- 每个连接只保留最后一次快照，引用（按绘制顺序的 `e1`、`e2`……）只对它有效。

## 操作

```json
{ "op": "ui", "snapshot": "ui-12", "action": "click", "ref": "e5" }
```

| `action` | 字段 | |
|---|---|---|
| `click` | `button`（`left`）、`holdMs`、`anchor`、`offsetX`/`offsetY`、`force` | 在控件上 `mouse click` |
| `setText` | `text`、`clear`（`true`）、`submit`（`false`）、`delayMs` | 点击获得焦点；`clear` 时 CTRL+A、BACK；再走 `text` 操作；`submit` 时按 ENTER |
| `scroll` | `notches`（`1`，正数向下） | 在控件上 `mouse wheel` |

`mouse` 操作可以用 `ref` + `snapshot` 代替 `x`/`y`，用于悬停和拖动：

```json
{ "op": "mouse", "action": "move", "ref": "e68", "snapshot": "ui-12" }
```

`anchor`（`center`、`top_left`、`top`、`top_right`、`left`、`right`、`bottom_left`、`bottom`、`bottom_right`）和 `offsetX`/`offsetY`（像素）决定落点，其余与普通 `mouse` 操作相同。

执行前 BeDD 会重新读取界面，按路径找到控件：

| `code` | |
|---|---|
| `stale_snapshot` | 不是本连接的最后一次快照（`snapshot` 给出最后一次） |
| `stale_ref` | 控件已消失或 hash 变化（`candidates`：当前最前界面上 role 相同、name 或 text 相同的控件）。请重新取快照 |
| `occluded` | 控件隐藏、在窗口外，或（对 `click`、`setText`、`scroll`、`mouse` 的 `down`/`click`）落点上有绘制在它之上的控件、或上层接受输入的界面中的控件（`occludedBy`）。`force:true` 强制执行 |
| `not_ready` | 客户端线程未及时响应，或还没有场景 |
| `unsupported` | Ore UI 节点，或本游戏版本的界面布局与预期不符 |

回复含 `point`、`target`（操作后的节点）、`screen`（最前面的界面）和 `changed`（文字、状态、位置或最前界面是否变化）。`observeMs`（`150`）是读取结果前等待游戏绘制的时间。

## BeDD 能读到什么（1.26.60）

| 界面 | 技术栈 |
|---|---|
| 标题（`start_screen`）、HUD（`hud_screen`）、聊天、暂停、背包、退出对话框 | JSON UI |
| 世界列表（`/play/all`）、设置（`/oreui-settings/...`） | Ore UI：一个带 `source.url` 和 `source.route` 的节点 |

悬停落在指针下最内层的控件上：背包中格子的 `grid_item_for_inventory` 里有一个显示 `hovered` 的 `item_button_ref`。界面的入场动画会短暂移动控件：请在界面稳定后再取快照。

游戏没有 RTTI，BeDD 通过虚表区分组件类型：文字组件靠其导出的 `render`，文本框、滑块和滚动视图来自 ScreenView 中的对应列表，按钮和开关靠它们指向的状态控件。1.26.60 的 JSON UI 界面里没有见到滑块和开关（设置界面是 Ore UI）。

## Ore UI 界面：`oreui`

`ui` 把 Ore UI 界面显示为一个节点。要看它的内部，发送 `{"op":"oreui"}`：BeDD 在 Ore UI 视图的页面中运行一段只读脚本，返回它的 DOM。

```json
{"id": 23, "op": "oreui"}
```

回复含 `url`（路由，如 `/play/all`）、`title`、`width`、`height`、按文档顺序排列的 `nodes` 和 `truncated`。节点有 `tag`，以及（存在时）`id`、`class`、`testId`（`data-testid`）、`role`、`name`（`aria-label`、`title`、`alt`、`placeholder` 或 `name`）、`text`（节点自身的文字）、`value`、`state`（`disabled`、`checked`、`selected`、`expanded`、`hidden`、`focused`、`invisible`）、以游戏客户区像素计的 `bounds`，以及 `children`（`nodes` 中的下标）。

Ore UI 节点没有 ref。要点击某个节点，向其 `bounds` 的中心发送 `action: "click"` 的 `mouse`。例如从世界列表打开一个世界：

```js
const dom = await call('oreui')
const tile = dom.nodes.find(n => n.text === 'My World' && n.bounds?.w > 0)
await call('mouse', { action: 'click', x: tile.bounds.x + tile.bounds.w / 2, y: tile.bounds.y + tile.bounds.h / 2 })
```

脚本在视图下一次运行自己的脚本时执行，Ore UI 界面每秒会运行很多次。`timeoutMs`（`3000`）是等待时长，`url` 等待 URL 包含它的视图；最前面没有 Ore UI 界面时，该操作以 `not_ready` 结束。`maxNodes`（`2000`）和 `maxText`（`200`）限制回复大小。

`oreui` 调用游戏 `v8.dll` 中的 V8 引擎（1.26.60 中为 V8 9）。遇到其他主版本时，它返回 `unsupported`，并在 `findings.v8` 中给出版本；`"force": true` 则仍然尝试。若对 V8 的调用崩溃，BeDD 会捕获它，并在游戏重启前关闭 `oreui`。
