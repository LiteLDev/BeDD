# 安装

## 下载

从 [Releases](https://github.com/LiteLDev/BeDD/releases) 下载 `bedd-client-windows-x64.zip`，解压到任意位置。
里面是一个 `BeDD` 文件夹：模块（`BeDD.dll`、`manifest.json`）、PowerShell 客户端和脚本。

- **用于测试**：测试框架在每次运行开始时把模块复制进游戏实例的 `mods\` 目录、结束后删掉（[接入测试框架](/zh/guide/harness)）。
- **手动试一下**：解压到该实例的 `mods\` 目录，得到 `mods\BeDD\BeDD.dll`，再写一个[配置](/zh/guide/configuration)，
  让其中的 `managerScript` 指向它的 `BeDDManager.ps1`。用完记得删掉这个文件夹：留下一份副本的话，正常启动的游戏会不理你的鼠标键盘，
  而且只显示在管理窗口里。

Be.D.D. 不是 lip 包，也不在 LeviMC 模组里上架。

## 系统要求

Minecraft 基岩版 **1.26.51**（GDK 版）和 LeviLamina **26.51.x**（客户端），装在用 LeviLauncher 安装的游戏实例里。
测试时用 `Start-BeDDGame.ps1` 启动这个实例，不走启动器（[多开](/zh/guide/several-games)）。只有测试机上的游戏需要装，
服务器和玩家都不用。

管理窗口需要 PowerShell 7（`pwsh`）。

## 让测试框架自己下载

测试框架可以自己下载 Be.D.D.，不必要求某个目录下事先有一份检出。已经有解压好的副本时，用 `BEDD_DIR` 指向它：

```powershell
$bedd = if ($env:BEDD_DIR) { $env:BEDD_DIR } else { Join-Path $env:LOCALAPPDATA 'BeDD\client' }
if (-not (Test-Path "$bedd\harness-shim.ps1")) {
    $zip = Join-Path $env:TEMP 'bedd.zip'
    Invoke-WebRequest https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-client-windows-x64.zip -OutFile $zip
    Expand-Archive $zip (Split-Path $bedd) -Force; Rename-Item (Join-Path (Split-Path $bedd) 'BeDD') $bedd
}
```

## 自己构建

见[构建与贡献](/zh/reference/building)。

## 下一步

电脑上还没有 LeviLauncher、LeviLamina 或游戏实例时，读[在新电脑上准备](/zh/guide/new-pc)；否则直接看[驱动游戏](/zh/guide/usage)。
