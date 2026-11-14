# 工作原理

| 部分 | 机制 |
|---|---|
| 传输 | 基于 Winsock 的 RFC 6455，SHA-1 来自 CNG；文本 = JSON，二进制 = [tap](/zh/reference/tap)；每条连接一个线程（[协议](/zh/reference/websocket)） |
| 按键、文字、指针 | 入队 + `PostMessage(hwnd, WM_LHI)`；子类化的 WndProc（游戏线程）设置虚拟状态，用 `WM_KEYDOWN`/`WM_KEYUP`、`WM_CHAR`、`WM_MOUSEMOVE` + 虚拟光标（钩住的 `GetCursorPos`）调用游戏的 WndProc，按真实输入处理（`MainGameCore::WndProc_PC::onKeyboardAction` → `HIDControllerGameCoreDesktop::onKeyDown`）。派发后回复 |
| 按钮、滚轮、视角 | GameInput v2（`GameInputRedist.dll`）忽略窗口消息：钩住 `IGameInputReading::GetMouseState`、`GetKeyCount`、`GetKeyState`；`GetCurrentReading` 在设备上报前取得虚拟读数 |
| 不抢前台 | `launch`：`CreateProcessW` + `SW_SHOWNOACTIVATE`；钩住 `ShowWindow`、`SetWindowPos`、`SetActiveWindow`、`BringWindowToTop`、`SetForegroundWindow`；Z 序最底；吞掉 `WM_ACTIVATEAPP(0)`/`WM_KILLFOCUS`/`WM_ACTIVATE(inactive)`（不暂停）。钩子在 `load()` 中安装（窗口在 `enable()` 之前已存在）。伪造的“获得焦点”跳过默认的 `WM_ACTIVATE`（钩住的 `DefWindowProcW`）。`BEDD_RETURN_FOCUS`：游戏启动时若被激活，要重新聚焦的窗口 |
| 屏蔽真实输入 | 吞掉 `WM_KEY*`/`WM_MOUSE*`/`WM_INPUT`/`WM_POINTER*`；虚拟化 `ClipCursor`（空操作）、`SetCursorPos`、`GetCursorPos`、`GetPhysicalCursorPos`、`GetAsyncKeyState`、`GetKeyState`、`GetKeyboardState`、`GetForegroundWindow`/`GetActiveWindow`/`GetFocus`（游戏窗口）、`GetRawInputData`/`GetRawInputBuffer`（丢弃） |
| 截图 | `PrintWindow(PW_RENDERFULLCONTENT)`（被遮挡/在屏幕外也可），WIC |
| 克隆 | 从不写入原实例（`LeviLauncher.exe instance list --json`）。`%LOCALAPPDATA%\BeDD\instances\<gameVersion>-<slot>`：硬链接（跨卷时复制），独立的 `mods\`、世界、选项；改动的文件整体替换，不原地编辑。加载器从 exe 所在文件夹解析（保持 exe 名）：数据在其 `Minecraft Bedrock` 中 |
| 管理器 | `BeDD.exe`，每个登录会话一个：DWM 缩略图；`launch` 在调用方进程树之外启动它；通过发现文件附加（[`manager`](/zh/reference/websocket#系统操作)）；关闭它会发送 `close`。游戏窗口：从创建起就是屏幕外的工具窗口（钩住 `CreateWindowExW`：`enable()` 前无消息泵），丢弃 `SetWindowPos` 移动，由监视器复位 |

## 游戏内的 bedboo 操作

| 操作 | 实现 |
|---|---|
| `world` | 界面字段每帧更新（`ClientInstance::$update` 后置钩子：暂停的世界不 tick），玩家每 tick 更新 |
| `look` | 每 tick 调用 `LocalPlayer::localPlayerTurn`；按 1:1 度转动，轴交换（`{-dPitch, dYaw}`）；下一 tick 到位 |
| `input` | 后置钩子 `ClientInputUpdateSystem::extractRawHIDInput`（`RawMoveInputComponent::mRawInput`/`mRawMove`）和 `inputHandlerUpdatePlayerState`（前后两次写 `MoveInputComponent::mInputState`/`mRawInputState`；`MoveInputState` `SneakDown`=0、`JumpDown`=7、`SprintDown`=8、`Up`..`Right`=13..16、边沿 21..26；`mAnalogMoveVector`）。覆盖结束后清一次标志（键盘只在事件时写入）。`tick` = `inputHandlerUpdatePlayerState` 调用次数 |
| `slot`、`action`、`button` | 客户端 tick 监听器；perspective：`ClientInputCallbacks::handleTogglePerspectiveButtonPress`（自 1.26.60 起唯一导出的 `handle*ButtonPress`） |
| `send`、`stack` | 客户端线程：`MinecraftPackets::createPacket` + `Packet::read`（客户端 NetworkSystem 的 `ReflectionCtx`），`sendNetworkPacket` |
| `stack` | 绕过 `ItemStackNetManagerClient`（全为 MCNAPI；依据界面控制器预测）。在钩住的 `ClientNetworkHandler::handle(ItemStackResponsePacket)` 上重新同步 |
| `join` | `ClientInstance::connectToThirdPartyServer` |
| `timescale` | 服务端经 `ServerInstance::queueForServerThread`，客户端在 `ClientInstance::update` 之后；写 `Timer::mTimeScale`（`setSimTimeScale` 会相乘，上限 10）；暂停用 `Minecraft::setSimTimePause`。服务端速度：钩住 `ClientNetworkHandler::handle(LevelEventPacket)`，在游戏处理之后，仅当尚未达到时 |
| `ui`、`oreui` | [UI 树](/zh/reference/ui#实现) |
