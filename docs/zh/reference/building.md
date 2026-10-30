# 构建与贡献

## 构建

需要 Windows x64、[xmake](https://xmake.io)、MSVC 14.4x 或更新的 Visual Studio 生成工具，以及 LLVM（`clang-cl`）。
`tools/build.ps1` 会找到它们，缺的用 winget 安装，然后构建：

```powershell
pwsh -NoProfile -File tools/build.ps1   # [-Mode debug]
```

手动构建：

```powershell
xmake f -a x64 -m release -p windows --target_type=client -y
xmake -y        # -> bin\BeDD\（DLL、manifest 以及 client\ 里的所有文件）
```

模组打包器从 `git describe` 读取版本号，所以仓库至少要有一个提交。

`tools\dev.ps1 [-Trace]` 是一个试验场：它拿到共享的 e2e 锁（`%APPDATA%\levilauncher.exe\bei-e2e.lock`），把模块部署进 BeDD 找到的实例
（新电脑上先准备好，见[在新电脑上准备](/zh/guide/new-pc)），没有管理窗口时先启动它，通过 LeviLauncher 启动游戏（游戏显示在管理窗口里），并把每次前台和光标锁定的变化记录到
`tools\out\desktop-watch.log`，直到游戏退出或 `tools\out\dev.stop` 存在（`tools\out\dev.restart` 会重新部署并重启，不释放锁）。
它最后总会关掉游戏、删掉模块并释放锁。

## 文档与展示素材

`docs/` 是文档站点（<https://docs.levimc.org/BeDD/>，由 `.github/workflows/docs.yml` 用 levimc-kit 构建；`cd docs; npm ci; npm run build`）。
`docs/public/showcase/` 存放站点的展示内容和 README 里的图片。

`tools/showcase/record.ps1 [-KeepOpen]` 重新录制桌面视频：在共享 e2e 锁（以及每个克隆的 `hi<N>-e2e.lock`）下，它准备三个克隆实例
（`-hi2`..`-hi4`，其他模组挪开），恢复 `tools/showcase/fixtures/Showcase.zip` 存档，在它自己的管理窗口里启动游戏（`BEDD_MANAGER_PIPE`，这样别的测试的游戏不会进画面）
并进入世界，把这个管理窗口放到背景（`stage.ps1`）上一块区域的左边三分之二，右边三分之一是运行 `demo.ps1` 的 Windows Terminal，然后用 ffmpeg 的 Desktop Duplication 抓取器录下真实屏幕的这块区域，存为
`tools/showcase/out/take-<n>.mkv`。带 `-KeepOpen` 时，创建 `tools/showcase/out/take` 开始下一次录制，`tools/showcase/out/stop` 结束。
`tools/showcase/encode.ps1` 把一次录制剪成 MP4、WebP 和封面图。

## 源码结构

| 文件 | |
|---|---|
| `src/BeDD.h` | 共享状态，以及下面各部分的声明 |
| `src/Mod.cpp` | 入口、配置、虚拟按键/光标状态 |
| `src/Window.cpp` | 窗口查找、子类化、合成事件分发、焦点屏蔽、让窗口留在屏幕外 |
| `src/ApiHooks.cpp` | user32 钩子（光标、按键状态、前台、激活、原始输入） |
| `src/GameInputHooks.cpp` | GameInput v2 钩子（虚拟鼠标、键盘读数） |
| `src/Audio.cpp` | FMOD 钩子：静音游戏的主通道组（`mute`） |
| `src/NetHooks.cpp` | 只用本机网络（`loopbackOnly`：没有防火墙提示，不查询本机以外的服务器） |
| `src/Pipe.cpp` | 控制管道和协议 |
| `src/Manager.cpp` | 与会话管理窗口的连接 |
| `src/Trace.cpp` | trace 模式的输入日志 |
| `src/MoveInput.cpp` | `input` 操作：在客户端的 `ClientInputUpdateSystem` 里覆盖移动，带死人开关 TTL（`gameHooks`） |
| `src/Send.cpp` | `send` 操作（`allowSend`）：允许列表内的服务端方向数据包，在客户端线程上由本地玩家发送 |
| `src/GameState.cpp`、`src/Look.cpp` | `world` 快照和绝对 `look`（`gameHooks`） |
| `src/GameActions.cpp` | `slot`、`action` 和按刻计算的 `button` 操作（`gameHooks`） |
| `src/Stack.cpp` | `stack` 操作（`allowStack`）：由本地客户端发送的物品堆请求 |
| `src/TapSink.h` | 所有 tap 发布者共用的帧类型和 sink 链（保持逐字节一致） |
| `src/Tap.cpp`、`src/Tap.h` | tap 管道：帧格式、标记、写线程、`tap` 操作 |
| `src/NetTap.cpp` | 远程服务器连接上的 tap 抓取，以及 blob 缓存开关 |
| `src/TapLoopback.cpp` | 单人世界和局域网主机世界里的 tap 抓取（重新序列化的包） |
| `src/TapJournal.cpp` | tap 的加入日志（给晚到读取方的压缩回放） |
| `src/MemoryOperators.cpp` | 让 `new`/`delete` 走 LeviLamina 的分配器（LeviLamina 要求） |
| `client/` | PowerShell 客户端、框架垫片、游戏启动（`Start-BeDDGame.ps1`）、管理窗口（`BeDDManager.ps1`）、克隆脚本和准备脚本（`BeDDSetup.psm1`、`Initialize-BeDD.ps1`：找到游戏实例并补齐新电脑缺的东西），都和 DLL 放在一起发布 |

挂钩游戏或 LeviLamina 的符号时，先查 `%LOCALAPPDATA%\.xmake\packages\l\levilamina\<version>\<hash>\include` 下的 SDK 头文件。
只调用 `MCAPI`、`MCFOLD` 和 `MCTAPI` 函数。

## Pull request

1. Fork 仓库，从 `main` 建分支。
2. 每个 pull request 只做一件事。说明它做了什么、你怎么测试的。
3. 遵循现有风格：C++20、仓库里的 `.clang-format` 和 `.editorconfig`，代码和注释用英文。
4. 在游戏里测试：`tools/dev.ps1 -Trace`，然后通过 `BeDD.psm1` 驱动游戏，检查输入有效、游戏从不抢前台（`tools/out/desktop-watch.log`），
   真实输入也到不了游戏。
5. CI 通过后才能合并。

提交信息用祈使语气（"Add wheel tilt"，而不是 "Added …"）。对用户可见的改动，在
[CHANGELOG.md](https://github.com/LiteLDev/BeDD/blob/main/CHANGELOG.md) 的 *Unreleased* 下记一笔。

另见 [CONTRIBUTING.md](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md)。参与贡献即表示你同意你的贡献以本项目的许可证
[LGPL-3.0-or-later](https://github.com/LiteLDev/BeDD/blob/main/COPYING.LESSER) 授权。
