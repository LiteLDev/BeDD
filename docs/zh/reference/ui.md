# UI 树

`ui` 把 JSON UI 和 Ore UI 界面读成文本，每个控件一行，并用 input 操作的虚拟鼠标、按键和文字对 ref 执行动作。在
bedboo 操作之后使用（[顺序](/zh/guide/operation-modes)）。配置 `ui`（`true`）同时控制 `oreui` 和带 `ref` 的 `mouse`。
设计：[#21](https://github.com/LiteLDev/BeDD/issues/21)。

1600x900 下的字符数：标题界面 519（旧 JSON 20,569），世界列表 ~860（原始 DOM ~48,000），暂停 ~600，背包 ~950。

## 视图

```
screen start_screen
- button "Play" [e1]
- button "Settings" [e2]
- text "v26.60/LeviLamina-26.60.5"
under: toast_screen, debug_screen, cubemap_background_screen
```

| 行 | |
|---|---|
| `screen <name>` | 每个读取的界面一行；Ore UI：`screen <route> (Ore UI)`，如 `screen /play/all (Ore UI)`。后缀 ` (not ready: read again)` 或 ` (page not read: <why>)` |
| `- <role> "<text>"` / `- <role> <name>` | 然后是 ` = <value>` 或 ` checked`/` unchecked`、` disabled`、` focused`、` selected`，可执行动作时再加 ` [<ref>]`。缩进 = 嵌套 |
| `- button hotbar_slot_button x9 [in e29]` | 连续 6 行以上相似的行；`{op:"ui", ref:"e29"}` 列出它们 |
| `... <n> lines cut: ...` | 超过 `maxNodes`；保留前 3/4 和后 1/4 |
| `under: a, b` | 最前面界面之下的界面（仅 `scope:"active"`） |
| `(nothing read on this page)` / `(no match)` | Ore UI 页面为空 / `find` 无命中 |

角色：`button`、`toggle`、`text_input`、`slider`、`scroll`、`text`、`item`（集合格子，如背包格）、`group`。

输出内容（`compact`）：能看到、能读或能操作的。按钮读作其第一个可见标签；重复其控件的文字被丢弃；分组仅在可点击且
不含其他行时输出；名为 `button`、`default`、`panel`、`image`、`label`、`content`、`control` 或其角色名的 JSON UI 控件
显示所在分组的名字。

Ref（`e1`、...）：每个控件（场景 + 路径）一个，在其界面留在栈上期间由所有读取和连接共用；从不重新分配。`snapshot`
字段被忽略。

## 读取

| 请求 | 回复 |
|---|---|
| `{op:"ui"}` | `{view, screen（最前面）, truncated}` |
| `{op:"ui", find:"quit"}` | 文字、名字或值包含该串（不分大小写）的行，平铺 |
| `{op:"ui", ref}` | `{view, truncated}`：该控件（带 ref）及其下全部，不折叠 |
| `{op:"ui", ref, detail:true}` | `{node}`：全部节点字段 + `ref`、`ancestors`（文字或名字，由近及远） |
| `{op:"ui", format:"json"}` | `snapshot`（仅供参考）、`tick`、`screens`、`active`、`roots`、按 ref 的 `nodes`（+`parent`、`children`）、`truncated`、`client{w,h}` |

| 字段 | 默认 | |
|---|---|---|
| `scope` | `active` | `active`、`all`、界面名或 `scene-…` id |
| `maxNodes` | 80 行；json：`uiMaxNodes`（1000） | 1..100000 |
| `depth` | 全部 | 每个界面根以下的层数 |
| `includeInvisible` | `false` | 也包括隐藏控件 |
| `compact` | `true` | `false`：全部控件 |
| `timeoutMs` | 3000 | 100..30000，之后 `not_ready` |
| `debug` | `false` | json/detail：组件、虚表、可见性位 |

| 节点字段 | |
|---|---|
| `role`、`name`、`text` | `name`：JSON UI 控件名；Ore UI 的 `aria-label`/`title`/`alt`/`placeholder`/`name`/`data-testid` |
| `value` | 文本框文字、滑块 0..1、开关状态 |
| `state` | `visible, enabled, hovered, focused, selected, pressed, editable, scrollable, checked`（null：非开关） |
| `bounds`、`boundsNorm` | 客户区像素（`x`/`y`）；比例（`rx`/`ry`） |
| `actions` | `click`、`setText`、`scroll` |
| `surface` | 界面 id |
| `source` | `kind` jsonui\|oreui（整个页面）\|dom；`path`（同名兄弟用 `name[k]`）、`type`（JSON UI 类型或标签）、`collectionName`/`collectionIndex`（如 `inventory_items`/9）、`url`、`route` |
| `hash` | 由 role、name、text、path、type、kind、url、route 计算 |

`screens[]`：`{id, kind jsonui|oreui|unknown, screen, interactive, modal}`，设置时另有 `rawScreen`、`route`、`ready:false`、
`pageError`、`debug`。`unknown` 场景（`in_game_play_screen`）不读取。

## 操作

```json
{"op": "ui", "action": "click", "ref": "e1"}
{"ok": true, "action": "click", "ref": "e1", "point": {"x": 800, "y": 428}, "changed": true,
 "target": "- button \"Play\" [e1]", "screen": "/play/all", "view": "screen /play/all (Ore UI)\n- button ..."}
```

| `action` | 字段（默认） | 输入 |
|---|---|---|
| `click` | `button`（left）、`holdMs`、`settleMs`、`afterMs` | `mouse click` |
| `setText` | `text`、`clear`（true）、`submit`（false）、`delayMs` | 点击；CTRL+A、BACK；`text`；ENTER |
| `scroll` | `notches`（1，正数 = 向下） | `mouse wheel` |

全部动作：`anchor`（center、top_left、top、top_right、left、right、bottom_left、bottom、bottom_right）、`offsetX`/`offsetY`、
`force`、`timeoutMs`、`observeMs`（150，≤5000：读取结果前等待）。

回复：`point`；`changed`（任何文字、状态、值、位置或最前面界面变化）；`target`（其当前行，消失则为 null）；`screen`
（最前面）；`view` 仅在另一个界面或 Ore UI 路由来到最前面时返回。

### 引用上的 mouse

`{"op":"mouse", "action":"move", "ref":"e68"}`：`mouse` 瞄准一个 ref（`anchor`、`offsetX`/`offsetY`、`force`；`action`
默认 `move`）；回复附加动作回复的字段。只有 `click`/`down` 检查遮挡控件。

### 错误

控件必须保持其角色和名字（JSON UI）或文字（Ore UI）。

| `code` | |
|---|---|
| `stale_ref` | 未知 ref、界面已关闭、控件消失或已改变。`candidates`：≤5 行角色和名字/文字相同的控件（其自身界面已关闭时取自最前面界面） |
| `occluded` | 隐藏；在窗口外（`point`）；或在该点被遮挡（`occludedBy`、`point`），遮挡者为其界面上更晚的输入控件（JSON UI）或上方接收输入的界面。≥ 半个窗口的控件不计。`force:true` 强制执行 |
| `not_ready` | `timeoutMs` 内无应答、无场景，或 Ore UI 页面无响应 |
| `unsupported` | ref 是整个 Ore UI 页面，或无法读取 UI |
| `bad_args` | `maxNodes`/`depth`/`timeoutMs` 不是整数、`format` 不是 text/json、`find`/`ref` 不是字符串 |
| `bad_request` | 动作缺 `ref`、未知 `action`、`setText` 缺 `text` |

## 1.26.60

JSON UI：标题（`start_screen`）、HUD（`hud_screen`）、聊天、暂停、背包、退出对话框；未见滑块或开关。Ore UI：世界列表
（`/play/all`）、设置（`/oreui-settings/...`）。悬停命中最内层控件（背包：`grid_item_for_inventory` 中的
`item_button_ref`）。等入场动画结束后再读。

## 实现

| 部分 | |
|---|---|
| JSON UI | SceneStack → `UIScene::mScreenView` → `VisualTree::mRootControl` → `UIControl::mChildren`/`mComponents`，在客户端线程（`ClientInstance::update` 钩子）。按虚表判断类型（无 RTTI）（文字：导出的 `render`；文本框/滑块/滚动：ScreenView 的列表；按钮/开关：4/8 个状态 weak_ptr）。SEH：布局异常 → `unsupported` |
| Ore UI | 最上层 Ore UI 页面的 DOM，经 `oreui` 脚本（≤4000 节点，120 字符），角色按标签、input `type` 和 ARIA role 判断（`domRole`，`src/UiView.cpp`）。无文字的控件取其内部文字；与有文字控件相邻的无文字 `act` 2 元素视为其一层 |

## Ore UI 界面：`oreui`

原始 Ore UI DOM，无 ref（调试用）。

| 字段 | 默认 | |
|---|---|---|
| `url` | 任意 | 等待 `location.href`（如 `ui://./hbui/index.html`）包含该串的视图；不是路由 |
| `maxNodes` | 2000 | 1..50000 |
| `maxText` | 200 | 8..4096 |
| `timeoutMs` | 3000 | 100..30000，之后 `not_ready` |
| `force` | `false` | 允许 9 以外的 V8 主版本 |

回复：`url`、`title`、`width`、`height`、`nodes`（文档顺序）、`truncated`、`findings`（`method`、`gameface`、`v8`、`jsCalls`、
`contextsSeen`、`missingExports`）。节点：`tag`、`id`、`class`、`testId`、`role`、`type`、`act`、`name`、`text`（自身）、
`value`、`state`（`disabled, checked, selected, expanded, hidden, focused, invisible`）、`bounds`（客户区像素）、`children`
（索引）。`act`：1 = 按标签、ARIA role、tabindex、test id 或 React/Preact 处理函数可点击；2 = 仅有 `-interactive` 悬停样式
（且不在 `act` 祖先内）。

钩住 `v8.dll` 中的 `v8::Function::Call`/`v8::Object::CallAsFunction`（V8 9.4；首次使用时），在页面下一次调用时（~300/s）
运行只读脚本；同一时间一个请求。`unsupported`（`findings`）：其他 V8 主版本、导出缺失、脚本错误、DOM > 15 MiB。V8 崩溃
（SEH）或脚本不结束会禁用 `oreui` 和 `ui` 中的 Ore UI，直到重启。
