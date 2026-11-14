# How it works

| Part | Mechanism |
|---|---|
| Transport | RFC 6455 on Winsock, SHA-1 from CNG; text = JSON, binary = [tap](/reference/tap); thread per connection ([protocol](/reference/websocket)) |
| Keys, text, pointer | queued + `PostMessage(hwnd, WM_LHI)`; subclassed WndProc (game thread) sets the virtual state, calls the game's with `WM_KEYDOWN`/`WM_KEYUP`, `WM_CHAR`, `WM_MOUSEMOVE` + virtual cursor (hooked `GetCursorPos`), handled as real input (`MainGameCore::WndProc_PC::onKeyboardAction` → `HIDControllerGameCoreDesktop::onKeyDown`). Replies after dispatch |
| Buttons, wheel, camera | GameInput v2 (`GameInputRedist.dll`) ignores window messages: hooked `IGameInputReading::GetMouseState`, `GetKeyCount`, `GetKeyState`; `GetCurrentReading` gets a virtual reading before the device reports |
| No foreground | `launch`: `CreateProcessW` + `SW_SHOWNOACTIVATE`; hooked `ShowWindow`, `SetWindowPos`, `SetActiveWindow`, `BringWindowToTop`, `SetForegroundWindow`; bottom of Z order; `WM_ACTIVATEAPP(0)`/`WM_KILLFOCUS`/`WM_ACTIVATE(inactive)` swallowed (no pause). Hooks in `load()` (window exists before `enable()`). Fake "focus gained" skips default `WM_ACTIVATE` (hooked `DefWindowProcW`). `BEDD_RETURN_FOCUS`: window to refocus if the game activates while starting |
| Real input blocked | `WM_KEY*`/`WM_MOUSE*`/`WM_INPUT`/`WM_POINTER*` swallowed; virtual `ClipCursor` (no-op), `SetCursorPos`, `GetCursorPos`, `GetPhysicalCursorPos`, `GetAsyncKeyState`, `GetKeyState`, `GetKeyboardState`, `GetForegroundWindow`/`GetActiveWindow`/`GetFocus` (game window), `GetRawInputData`/`GetRawInputBuffer` (dropped) |
| Screenshot | `PrintWindow(PW_RENDERFULLCONTENT)` (covered/off screen OK), WIC |
| Clones | The LeviLauncher instance (found with `LeviLauncher.exe instance list --json`) is never written to. `%LOCALAPPDATA%\BeDD\instances\<gameVersion>-<slot>`: hardlinks (copies across volumes), own `mods\`, worlds, options; changed files replaced, not edited. Loader resolves from the exe folder (keep the exe name): data in its `Minecraft Bedrock` |
| Manager | `BeDD.exe`, one per logon session: DWM thumbnails; `launch` starts it outside the caller's tree; attaches via discovery files ([`manager`](/reference/websocket#system-ops)); closing it sends `close`. Game window: off-screen tool window from creation (hooked `CreateWindowExW`: no pump before `enable()`), `SetWindowPos` moves dropped, a watcher restores |

## bedboo ops inside the game

| op | Implementation |
|---|---|
| `world` | screen fields per frame (`ClientInstance::$update` post-hook: paused worlds don't tick), player per tick |
| `look` | `LocalPlayer::localPlayerTurn` per tick; turns 1:1 degrees, axes swapped (`{-dPitch, dYaw}`); lands next tick |
| `input` | post-hooks `ClientInputUpdateSystem::extractRawHIDInput` (`RawMoveInputComponent::mRawInput`/`mRawMove`) and `inputHandlerUpdatePlayerState` (`MoveInputComponent::mInputState`/`mRawInputState` before and after; `MoveInputState` `SneakDown`=0, `JumpDown`=7, `SprintDown`=8, `Up`..`Right`=13..16, edges 21..26; `mAnalogMoveVector`). Flags cleared once after the override (keyboard writes only on events). `tick` = `inputHandlerUpdatePlayerState` calls |
| `slot`, `action`, `button` | client tick listener; perspective: `ClientInputCallbacks::handleTogglePerspectiveButtonPress` (the only `handle*ButtonPress` exported since 1.26.60) |
| `send`, `stack` | client thread: `MinecraftPackets::createPacket` + `Packet::read` (client NetworkSystem `ReflectionCtx`), `sendNetworkPacket` |
| `stack` | bypasses `ItemStackNetManagerClient` (all MCNAPI; predicts from screen controllers). Resync on hooked `ClientNetworkHandler::handle(ItemStackResponsePacket)` |
| `join` | `ClientInstance::connectToThirdPartyServer` |
| `timescale` | server via `ServerInstance::queueForServerThread`, client after `ClientInstance::update`; writes `Timer::mTimeScale` (`setSimTimeScale` multiplies, caps at 10); pause `Minecraft::setSimTimePause`. Server scale: hooked `ClientNetworkHandler::handle(LevelEventPacket)`, after the game, only if not already reached |
| `ui`, `oreui` | [UI tree](/reference/ui#implementation) |
