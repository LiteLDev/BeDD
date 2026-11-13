# 工作原理

```
 test script / agent / bedboo             Minecraft.Windows.exe + BeDD.dll
 ────────────────────────────             ─────────────────────────────────────────────
 {"id":1,"op":"key","key":"E"} ──text frame──▶ ws://127.0.0.1:<port>/v1   (token)
                                          │ queue Event{WM_KEYDOWN 'E'}; PostMessage(hwnd, WM_LHI)
                                          ▼
                                   subclassed window procedure (game thread)
                                     WM_LHI → apply virtual key/cursor state
                                            → CallWindowProc(game's WndProc, WM_KEYDOWN, ...)
                                     real WM_KEY*/WM_MOUSE*/WM_INPUT/WM_POINTER* → swallowed
                                     WM_ACTIVATEAPP(0)/WM_KILLFOCUS/WM_ACTIVATE(inactive) → swallowed
 ◀──{"id":1,"ok":true}── after the game thread dispatched it
```

1. **传输层是回环地址上的 WebSocket。** 游戏加载模块后，模块在 `127.0.0.1` 上监听（配置 `wsPort`，默认任一空闲端口），路径 `/v1`，
   并写入 `%LOCALAPPDATA%\BeDD\run\<pid>.json`，内含 `url`、一个随机的 32 字节 `token` 和实例名；退出时删除这个文件，
   进程已不在的文件读取方会忽略。客户端以 `Authorization: Bearer <token>` 或 `?token=<token>` 带上令牌；没带的请求得到 HTTP 401，
   带 `Origin` 头的请求（网页发起的）得到 403，除非配置 `allowOrigins` 列出了它。服务端是基于 Winsock 的一个小型 RFC 6455 实现
   （SHA-1 来自 CNG）：文本帧承载 JSON 请求和回复，二进制帧承载 [tap](/zh/reference/tap)。每个连接有自己的线程、按顺序处理；
   可以同时打开多个连接（`wsMaxClients`，默认 8），某个连接断开时，它按住的键、按钮和 `input` 覆盖都会松开。协议见
   [`spec/bedd.asyncapi.yaml`](https://github.com/LiteLDev/BeDD/blob/main/spec/bedd.asyncapi.yaml)，说明在
   [WebSocket 参考](/zh/reference/websocket)。
2. **三种驱动游戏的方式，从语义到像素。** *bedboo* 操作（`world`、`look`、`input`、`slot`、`action`、`button`、`send`、`stack` 等）
   挂钩游戏自己的系统，在客户端刻上生效，与按键绑定无关。*界面组件树*操作 `ui` 把场景栈上各界面的 JSON UI 控件读成带 ref 的
   无障碍风格的树，并通过下面的虚拟鼠标和按键对某个 ref 执行操作。*输入*操作（`key`、`text`、`mouse`、`raw`、`screenshot`）
   就是玩家的手和眼睛做的事；下面几条讲它们怎么到达游戏。
3. **键盘、文字和指针位置走游戏自己的窗口过程。** 模块子类化游戏窗口。按键变成 `WM_KEYDOWN`/`WM_KEYUP`，文字变成
   `WM_CHAR`，指针变成 `WM_MOUSEMOVE` 加一个虚拟光标（被挂钩的 `GetCursorPos` 返回的位置），在游戏线程上按顺序分发。
   游戏把它们当作真实输入处理（`MainGameCore::WndProc_PC::onKeyboardAction` → `HIDControllerGameCoreDesktop::onKeyDown`），
   所以聊天、键位映射和界面悬停都能用。游戏线程处理完输入后才回复，所以脚本的结果是确定的。
4. **鼠标按钮、滚轮和视角走虚拟 GameInput 鼠标。** 基岩版用 GDK 的 **GameInput** API（`GameInputRedist.dll`，接口 v2）
   直接从设备读取鼠标：`WM_LBUTTONDOWN` 之类的窗口消息会被忽略；没有模块的话，你真实的鼠标仍会在后台游戏里点击、转视角。
   模块挂钩 `IGameInputReading::GetMouseState`（以及 `GetKeyCount`/`GetKeyState`）：游戏读到的是虚拟按钮、
   相对位移（视角）、滚轮和绝对位置。设备报告输入之前，GameInput 没有该鼠标的读数，所以在没人碰过的电脑上，
   游戏的 `GetCurrentReading` 会从模块拿到该设备的一个虚拟读数。
5. **游戏从不抢前台，也不会失去焦点。** 失去焦点的消息被吞掉，所以游戏不会暂停或松开按键。`BeDD.exe launch` 用
   `CreateProcessW` 和 `SW_SHOWNOACTIVATE` 启动 exe；被挂钩的 `ShowWindow`/`SetWindowPos`/`SetActiveWindow`/`BringWindowToTop`/`SetForegroundWindow`
   从不激活游戏窗口，窗口一直保持在 Z 序最底部。user32 钩子在 `load()` 里就装好了：游戏在 `enable()` 之前就创建并显示窗口。
   模块发送的"获得焦点"消息会跳过默认的 `WM_ACTIVATE` 处理（挂钩 `DefWindowProcW`），否则它会设置焦点从而激活窗口：
   从用户前台应用（终端、agent）启动的游戏被允许抢前台。
6. **你的真实输入永远到不了游戏。** 真实的客户区输入消息被吞掉，游戏用来读取或锁定设备的 user32 调用看到的是*虚拟*状态：
   `ClipCursor`（空操作：你的光标从不被锁住）、`SetCursorPos`（只移动虚拟光标）、`GetCursorPos`/`GetPhysicalCursorPos`、
   `GetAsyncKeyState`、`GetKeyState`、`GetKeyboardState`、`GetForegroundWindow`/`GetActiveWindow`/`GetFocus`（报告游戏窗口）、
   `GetRawInputData`/`GetRawInputBuffer`（真实的原始输入被丢弃）。
7. **截图**（`screenshot` 操作）对游戏窗口用 `PrintWindow(PW_RENDERFULLCONTENT)`，窗口被挡住时也能用，结果以 base64 的 PNG 或 JPEG 返回，
   并缩小到 `maxWidth`（默认 960）以内，省 token。
8. **每个游戏都是克隆。** `BeDD.exe` 读取用户的 LeviLauncher 实例（`LeviLauncher.exe instance list --json`），从不往里写。
   每个槽位是 `%LOCALAPPDATA%\BeDD\instances\<游戏版本>-<槽位>` 下的一个克隆：游戏文件是指向实例文件的硬链接（跨卷时复制），
   `mods\`、存档和设置是它自己的；要改的文件会被替换（删掉再新写），从不原地修改，所以实例里的那份永远不会被动到。
   打过补丁的 `Minecraft.Windows.exe` 里 LeviLauncher 的加载器按 exe 所在目录解析一切，所以只要 exe 保持这个名字，
   克隆的数据就留在它自己的 `Minecraft Bedrock` 目录里。
9. **管理窗口**（不带参数运行的 `BeDD.exe`，每个登录会话一个）通过 DWM 缩略图显示每个游戏，游戏窗口在屏幕外时缩略图仍是实时的；
   游戏只显示在这里。没有管理窗口时 `BeDD.exe launch` 会启动一个，且不在调用者的进程树里，所以启动它的 agent 或测试框架退出后它仍然存在。
   它通过发现文件找到游戏，并通过各自的 WebSocket 接管（`{"op":"manager","action":"attach"}`）。游戏创建窗口后 30 秒内没被管理窗口接管、
   或者接管它的连接断开，游戏就会自行关闭（配置 `manager`，默认 `true`；`false` 让游戏隐藏着运行、不需要管理窗口，用于 CI）。
   游戏总是把窗口创建为屏幕外的工具窗口（挂钩 `CreateWindowExW`：游戏在 `enable()` 之前不处理消息，所以在那之前无法从它的线程外修改窗口），
   并一直留在那里（`SetWindowPos` 的移动被丢弃，一个监视器会把它放回去）。关闭管理窗口会关闭它的游戏（`close` 操作）。

环境变量 `BEDD_LABEL` 给游戏在管理窗口里起名，`BEDD_RETURN_FOCUS`（一个窗口句柄，由 `BeDD.exe launch` 设置）告诉模块：
游戏启动时若仍激活了自己，把前台还给哪个窗口。
