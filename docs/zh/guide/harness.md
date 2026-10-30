# 接入测试框架

`harness-shim.ps1` 原地替换 LeviLauncher 测试框架的常用函数（`Focus-Game`、`Assert-GameForeground`、
`Set-GameWindowSize`、`Move-Mouse`、`Click`、`Scroll`、`Press-Key`、`Type-Text`、`Get-ClientBitmap`，参数不变），
改为通过控制管道与游戏通信，并包装 `Start-Game`/`Stop-Game`。测试场景不用改。在框架自己的 `lib.ps1` 之后、
脚本顶层 dot-source 它：

```powershell
. "$PSScriptRoot/lib.ps1"
. "$beddDir\harness-shim.ps1"
Register-BeDDNative ([MyE2E.Native])   # 可选，见下文
# 部署完你的模组、把其他模组挪开之后：
Install-BeDDMod -ModsDir $script:ModsDir -PipeName 'BeDD-<实例名>'
# ……在 finally 里、游戏关掉之后：
Uninstall-BeDDMod -ModsDir $script:ModsDir
```

`$beddDir` 是 Be.D.D. 解压的位置；测试框架可以[自己下载](/zh/guide/installation#让测试框架自己下载)。
`Initialize-BeDDInstance [-Slot N]` 返回要用的实例（`Dir`、`ModsDir`、`PipeName`），并[补齐](/zh/guide/new-pc)电脑上还缺的东西。

游戏用 `Start-BeDDGame.ps1` 启动，不要走启动器；"把其他模组挪开"那一步要跳过 `BeDD`：它每次运行都重新装、
结束就删（留下一份的话，正常启动的游戏也会进管理窗口）。

## 垫片提供的函数

| 函数 | |
|---|---|
| `Install-BeDDMod -ModsDir <目录> [-PipeName p] [-Trace]` | 把模块复制到 `<目录>\BeDD`，写好它的 `config.json`（窗口大小取 `$script:WindowW`/`$script:WindowH`，默认 1600×900；`managerScript` 用来启动管理窗口），游戏随后在管理窗口里无头运行，把 LeviLamina 切到内置崩溃记录器，并让客户端指向这条管道 |
| `Uninstall-BeDDMod -ModsDir <目录>` | 再删掉模块，游戏还占着 DLL 时会重试。放在 `finally` 里调用 |
| `Set-WorldSinglePlayer <存档目录>` | 把测试存档设成单人（不开多人、局域网、Xbox Live 或平台广播）。恢复测试存档后马上调用 |
| `Register-BeDDNative <类型>` | 重定向框架自己的原生输入类的方法（见下文） |
| `Start-Game` / `Stop-Game` | 被包装：游戏运行期间持有共享的信令锁（`$script:ExclusiveSignaling = $true` 时为独占），没有管理窗口时先启动它（启动不了时 `Start-Game` 抛出异常），游戏启动时若抢了前台，会把前台还给你原来的窗口 |

## 框架自己的输入类

如果测试场景直接调用框架自己的输入类（比如 `[MyE2E.Native]::Key(0x10, $false)`），给这个类加上下面几个字段，
并在每个方法开头加一行判断，`Register-BeDDNative` 会把它们填好：

```csharp
public static Action<ushort, bool> KeyRedirect;          // (vk, up)
public static Action<int, bool>    MouseButtonRedirect;  // (0 左 / 1 右 / 2 中, up)
public static Action<int>          WheelRedirect;        // (delta，每格 120)
public static Action<char>         UnicodeRedirect;
public static Action<int, int>     MoveRelativeRedirect; // 视角
public static void Key(ushort vk, bool up) { if (KeyRedirect != null) { KeyRedirect(vk, up); return; } /* 原来的 SendInput */ }
```

## 示例

完整的测试框架可以参考 [BeetrootSkin](https://github.com/LiteLDev/BeetrootSkin/tree/main/tools/e2e) 和
[AlleviatedBackpacks](https://github.com/LiteLDev/AlleviatedBackpacks/tree/main/tools/e2e)。
