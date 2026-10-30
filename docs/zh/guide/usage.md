# 驱动游戏

PowerShell 模块 `BeDD.psm1` 就放在模块旁边，通过游戏的控制管道与它通信（默认 `\\.\pipe\BeDD`，配置里可以另起名字）：

```powershell
Import-Module .\mods\BeDD\BeDD.psm1
Connect-BeDD                               # 等游戏启动好
Send-BeDDKey E                             # 打开背包
Send-BeDDMouse click -RX 0.5 -RY 0.474     # 按窗口比例位置点击
Send-BeDDKey T; Send-BeDDText '/give @s apple 3'; Send-BeDDKey ENTER
Save-BeDDScreenshot C:\temp\shot.png       # 窗口被挡住也能截
```

每个输入调用都等游戏线程处理完输入才返回（或抛出异常）。这些调用都不会移动真实光标、按下真实按键或改变前台窗口。

![指哪儿悬停，指哪儿点：虚拟光标能触发物品提示、点击格子](/showcase/inventory-hover.webp)

## 命令

| 命令 | 作用 |
|---|---|
| `Connect-BeDD [-PipeName p]` | 等游戏就绪，并选定之后的调用发给哪个游戏 |
| `Send-BeDDKey E` | 按一下键；`-Action down`/`up` 按住或松开，`-Vk 0x10` 指定任意虚拟键码 |
| `Send-BeDDText '...'` | 输入文字（不产生按键，所以 `t` 不会打开聊天栏）；`-DelayMs` 设定每个字的间隔 |
| `Send-BeDDMouse click -RX 0.5 -RY 0.5` | `move`、`down`、`up`、`click`、`wheel`，位置用 `-X -Y` 像素或 `-RX -RY` 比例；`-Button right` 右键 |
| `Send-BeDDRaw move -Dx 200` | 在世界里转视角；`click -Button right` 使用物品 |
| `Clear-BeDDInput` | 松开所有还按着的键和鼠标按钮 |
| `Save-BeDDScreenshot path.png` / `Get-BeDDBitmap` | 游戏画面，被挡住也照样截 |
| `Set-BeDDWindow 1600 900` | 调整游戏窗口大小，不会把它提到前面 |
| `Set-BeDDBackground $false` | 把游戏交还给你的真鼠标和键盘 |
| `Get-BeDDState` | 游戏是否在线、收到了多少输入、是否抢过前台 |
| `Get-BeDDWorld` | 是否在世界中、当前界面、鼠标是否锁定、是否打开菜单、玩家位置与朝向（`gameHooks`） |
| `Set-BeDDLook 90 -30` | 把玩家转到指定的偏航角和俯仰角（度，`gameHooks`） |
| `Set-BeDDInput -Z 1 -Sprint -TtlMs 2000` | 通过游戏自己的输入系统让玩家行走、跳跃、潜行或疾跑一段时间，与按键绑定无关（`gameHooks`） |
| `Set-BeDDSlot 3` / `Invoke-BeDDAction inventory` | 选择快捷栏格子；丢弃、全部丢弃、打开背包、切换视角、暂停（`gameHooks`） |
| `Set-BeDDButton attack -Action down -ReleaseOnBlockChange` | 按游戏刻或直到目标方块破坏为止按住攻击/使用/选取，不阻塞管道（`gameHooks`） |
| `Send-BeDDPacket -Base64 ...` | 让玩家发送脚本序列化好的聊天或命令数据包（`allowSend`） |
| `Send-BeDDStack -Buffer ...` / `-View` | 让玩家发送脚本序列化好的物品堆请求；`-View` 显示客户端看到的背包（`allowStack`） |

标着 `gameHooks`、`allowSend` 或 `allowStack` 的命令需要打开对应的[配置项](/zh/guide/configuration)；它们是为
[bedboo](https://github.com/LiteLDev/bedboo) 的 `bedd` 传输准备的。`Invoke-BeDD @{ op = '...' }` 可以原样发送
[控制管道协议](/zh/reference/protocol)里的任意请求。

## 按键名

字母、数字、`ESC`、`ENTER`、`BACK`、`TAB`、`SPACE`、`SHIFT`、`CTRL`、`ALT`、方向键（`LEFT`、`UP`、`RIGHT`、`DOWN`）、
`HOME`、`END`、`PGUP`、`PGDN`、`INSERT`、`DELETE`、`F1`..`F24`、`SLASH`、`GRAVE`、`MINUS`、`EQUALS`。其他键用 `-Vk`
传虚拟键码。

## 位置

`Send-BeDDMouse` 的坐标在游戏窗口客户区内：`-X -Y` 是像素，`-RX -RY` 是宽高的比例。固定好窗口大小（配置
`clientWidth`/`clientHeight`，或 `Set-BeDDWindow`），每次运行点击位置才一样。

鼠标按钮和滚轮发给一个虚拟鼠标，游戏读它而不读你的鼠标。`Send-BeDDRaw` 按原始计数移动这个鼠标，游戏锁定鼠标时
（在世界里、没打开界面）就是转视角。

## 聊天和命令

![Send-BeDDText 在聊天栏里输入 /summon sheep 和 /summon cow](/showcase/chat-summon.webp)

`Send-BeDDText` 以字符形式输入文字，不产生按键，所以先用 `Send-BeDDKey T`（或 `SLASH`）打开聊天栏，再用
`Send-BeDDKey ENTER` 发送。这样输入的命令，游戏就像玩家亲手输入一样执行。

## 截图

`Save-BeDDScreenshot` 和 `Get-BeDDBitmap` 在窗口被其他窗口挡住或移出屏幕时也能截到游戏画面。

## 选哪个游戏

`Connect-BeDD -PipeName <名字>` 选定之后的调用发给哪个游戏；`BEDD_PIPE` 设定默认值。同时运行多个游戏时，每个实例要有自己的管道名：
见[多开](/zh/guide/several-games)。

其他脚本也可以同时使用同一个游戏的管道（配置 `pipeInstances`）。某个脚本断开或进程退出时，它还按着的键和按钮会被松开。

## 接管游戏

在游戏窗口里用你的真键盘按 **Ctrl+Shift+F12** 会结束后台模式：你的鼠标键盘又能操作游戏了。脚本里用
`Set-BeDDBackground $false` 效果一样；管理窗口的右键菜单也可以接管游戏、再交还给脚本。
