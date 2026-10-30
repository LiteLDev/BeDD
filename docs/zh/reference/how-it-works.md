# 工作原理

```
 test script (pwsh)                      Minecraft.Windows.exe + BeDD.dll
 ──────────────────                      ─────────────────────────────────────────────
 Send-BeDDKey E ──JSON line──▶ \\.\pipe\BeDD
                                          │ queue Event{WM_KEYDOWN 'E'}; PostMessage(hwnd, WM_LHI)
                                          ▼
                                   subclassed window procedure (game thread)
                                     WM_LHI → apply virtual key/cursor state
                                            → CallWindowProc(game's WndProc, WM_KEYDOWN, ...)
                                     real WM_KEY*/WM_MOUSE*/WM_INPUT/WM_POINTER* → swallowed
                                     WM_ACTIVATEAPP(0)/WM_KILLFOCUS/WM_ACTIVATE(inactive) → swallowed
 ◀──{"ok":true}── after the game thread dispatched it
```

1. **键盘、文字和指针位置走游戏自己的窗口过程。** 模块子类化游戏窗口。按键变成 `WM_KEYDOWN`/`WM_KEYUP`，文字变成
   `WM_CHAR`，指针变成 `WM_MOUSEMOVE` 加一个虚拟光标（被挂钩的 `GetCursorPos` 返回的位置），在游戏线程上按顺序分发。
   游戏把它们当作真实输入处理（`MainGameCore::WndProc_PC::onKeyboardAction` → `HIDControllerGameCoreDesktop::onKeyDown`），
   所以聊天、键位映射和界面悬停都能用。游戏线程处理完输入后才回复，所以脚本的结果是确定的。
2. **鼠标按钮、滚轮和视角走虚拟 GameInput 鼠标。** 基岩版用 GDK 的 **GameInput** API（`GameInputRedist.dll`，接口 v2）
   直接从设备读取鼠标：`WM_LBUTTONDOWN` 之类的窗口消息会被忽略；没有模块的话，你真实的鼠标仍会在后台游戏里点击、转视角。
   模块挂钩 `IGameInputReading::GetMouseState`（以及 `GetKeyCount`/`GetKeyState`）：游戏读到的是虚拟按钮、
   相对位移（视角）、滚轮和绝对位置。设备报告输入之前，GameInput 没有该鼠标的读数，所以在没人碰过的电脑上，
   游戏的 `GetCurrentReading` 会从模块拿到该设备的一个虚拟读数。
3. **游戏从不抢前台，也不会失去焦点。** 失去焦点的消息被吞掉，所以游戏不会暂停或松开按键。`Start-BeDDGame.ps1` 用
   `SW_SHOWNOACTIVATE` 启动 exe；被挂钩的 `ShowWindow`/`SetWindowPos`/`SetActiveWindow`/`BringWindowToTop`/`SetForegroundWindow`
   从不激活游戏窗口，窗口一直保持在 Z 序最底部。user32 钩子在 `load()` 里就装好了：游戏在 `enable()` 之前就创建并显示窗口。
   模块发送的"获得焦点"消息会跳过默认的 `WM_ACTIVATE` 处理（挂钩 `DefWindowProcW`），否则它会设置焦点从而激活窗口：
   从用户前台应用（终端、agent）启动的游戏被允许抢前台。
4. **你的真实输入永远到不了游戏。** 真实的客户区输入消息被吞掉，游戏用来读取或锁定设备的 user32 调用看到的是*虚拟*状态：
   `ClipCursor`（空操作：你的光标从不被锁住）、`SetCursorPos`（只移动虚拟光标）、`GetCursorPos`/`GetPhysicalCursorPos`、
   `GetAsyncKeyState`、`GetKeyState`、`GetKeyboardState`、`GetForegroundWindow`/`GetActiveWindow`/`GetFocus`（报告游戏窗口）、
   `GetRawInputData`/`GetRawInputBuffer`（真实的原始输入被丢弃）。
5. **截图**用 `PrintWindow(PW_RENDERFULLCONTENT)`，窗口被挡住时也能用。
6. **管理窗口**（`client/BeDDManager.ps1`，用 `Add-Type` 编译的 C# WinForms，每个登录会话一个）通过 DWM 缩略图显示每个游戏，
   游戏窗口在屏幕外时缩略图仍是实时的；游戏只显示在这里。`Start-BeDDGame.ps1` 通过 WMI（在 `conhost --headless` 里调用 `Win32_Process.Create`）
   启动它，所以它不属于调用者的进程树或作业，启动它的 agent 或测试框架退出后它仍然存在。模块在 `load()` 里连接它，
   没有管理窗口时就启动一个（配置 `managerScript`：`pwsh -File <它> -Ensure`）；90 秒内没有管理窗口连上、或者彻底失去连接的游戏会自行关闭。
   游戏总是把窗口创建为屏幕外的工具窗口（挂钩 `CreateWindowExW`：游戏在 `enable()` 之前不处理消息，所以在那之前无法从它的线程外修改窗口），
   并一直留在那里（`SetWindowPos` 的移动被丢弃，一个监视器会把它放回去）。管理窗口把每个已连接的游戏放进它自己的
   kill-on-close 作业（连接之后才放，启动过程不受影响；一分钟内没连接的游戏之后也会加入）：关闭或结束管理窗口就会关闭这些游戏。

环境变量 `BEDD_RETURN_FOCUS`（一个窗口句柄，由 `Start-BeDDGame.ps1` 和垫片的 `Start-Game` 设置）告诉模块：游戏启动时若仍激活了自己，
把前台还给哪个窗口。
