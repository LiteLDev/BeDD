# 安装

## 安装

从 [Releases](https://github.com/LiteLDev/BeDD/releases) 下载：

| | |
|---|---|
| `bedd-windows-x64-setup.exe` | 安装程序 |
| `bedd-windows-x64.msi` | 静默/统一部署：`msiexec /i bedd-windows-x64.msi /qn` |
| `bedd-windows-x64.zip` | 解压到任意位置，运行 `BeDD.exe` |

安装程序不需要管理员权限：按用户装到 `%LOCALAPPDATA%\Programs\BeDD`，开始菜单加 **BeDD**（管理窗口），目录加进
`PATH`。内容：`BeDD.exe`、`mod\BeDD\`（BeDD.dll、manifest.json）、`mod\BeDD-server\`、`spec\bedd.asyncapi.yaml`。
`BeDD.exe` 是自包含的。

```powershell
BeDD.exe launch --slot 1 --json    # clone 1.26.60.05-1, install the mod, start; prints url, token
```

### 升级

运行新版安装程序；它会关掉开着的管理窗口及其游戏。之后每次 `launch` 在克隆缺模组时安装，模组与来源不同时替换（保留
`config.json`）：

- 默认来源：`BeDD.exe` 旁的 `mod\BeDD`，否则是当前目录所在检出的 `bin\BeDD`；
- `mod install <instance> --from <dir>` 之后：该目录（记在 `mods\BeDD\source.pin`）；目录里不再有构建时，`launch`
  发出警告，不动模组。不带 `--from` 的 `mod install` 结束固定。

不替换：`launch --keep-mod`，或该克隆有游戏在运行时。

### 卸载

设置 > 应用 > 已安装的应用 > **BeDD** 删除程序、开始菜单项和 `PATH` 项。要连 `%LOCALAPPDATA%\BeDD`（克隆、存档、日志）
一起删：对每个克隆运行 `BeDD.exe instance remove --slot N --yes`，再删除该目录。

## 让测试框架自己下载

`BEDD_DIR` 指向解压好的一份。CI 里固定版本（`releases/download/v0.4.0/...`）。

```powershell
curl.exe -L -o "$env:TEMP\bedd.zip" https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-windows-x64.zip
mkdir "$env:LOCALAPPDATA\BeDD\app" -Force; tar.exe -xf "$env:TEMP\bedd.zip" -C "$env:LOCALAPPDATA\BeDD\app"
```

## 自己构建

[构建与贡献](/zh/reference/building)。
