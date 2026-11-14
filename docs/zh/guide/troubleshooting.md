# 常见问题

| 现象 | 原因 → 解决 |
|---|---|
| HTTP 401 | 令牌错误或过期（每次启动都换新）→ 从 `ps --json` 同时取 `url` 和 `token` |
| HTTP 403 | `Origin` 请求头 → 去掉它，或列进 `allowOrigins` |
| 连接被拒绝 | 还没启动好或已退出 → 不带 `--detach` 运行 `launch` |
| 连接太多 | 超过 `wsMaxClients`（8）→ 关掉一些 |
| 游戏约 30 秒后关闭 | 没有管理窗口（见 `latest.log`）→ `manager status`、`logs\manager.log`，去掉 `--no-manager`；CI：配置 `manager=false` |
| 克隆要几分钟、几 GB | 跨卷复制 → 把 `BEDD_INSTANCES_DIR` 设到基础实例所在盘，重新克隆（[克隆](/zh/guide/new-pc#克隆)） |
| 找不到 LeviLauncher | 没装或低于 1.1 → 更新，或设 `BEDD_LEVILAUNCHER` |
| 找不到实例 | → `LeviLauncher.exe install 1.26.60 --loader`；有多个：`BEDD_INSTANCE`；见 `instance list --all` |
| 没有 WebSocket，模组没加载 | 不是 1.26.60 + LeviLamina 26.60.x（`latest.log`）→ `LeviLauncher.exe loader install`、`instance refresh` |
| 克隆里的 BeDD 模组旧了或不对 | `--keep-mod`、游戏在运行或 `--from` 固定 → `mod install <instance>`（[升级](/zh/guide/installation#升级)） |
| "发生错误"（"Blaze"） | 共用登录 → `world singleplayer`；在线：`--exclusive-signaling` |
| 游戏用了我平时的存档和设置 | 不是用 `launch` 启动的，或基础实例不是 LeviLauncher 1.1+ 装的 |
| 每个克隆都弹防火墙提示 | `loopbackOnly` 关了 → 打开；没有 BeDD 的服务器会问一次（点取消） |
| 启动时闪控制台窗口 | `CrashLogger.exe` → `launch` 会修好（[配置](/zh/guide/configuration)） |
| 关掉了别人的游戏 | 按进程名找 → 按实例找；每次运行一个槽位 |
| 断开后 `button` 没起作用 | 无终点的 `down` 被松开 → 用 `click`/`holdTicks`/`releaseOn`（[多条连接](/zh/guide/usage#多条连接)） |
| `look` 超时，游戏操作无反应 | 世界暂停（不走刻）→ `key ESC` |
| `ui` `stale_ref` / `occluded` / `not_ready` / `unsupported` | → 重新读 `ui` / `force:true` / 重试 / [模拟输入](/zh/guide/operation-modes#模拟输入)（[UI 树](/zh/reference/ui)） |
| `code:"disabled"` | [功能开关](/zh/guide/configuration#功能开关)为 `false`；`unknown op`：本版本没有该操作 |

## 报告 bug

[提 issue](https://github.com/LiteLDev/BeDD/issues/new/choose)，附上 `BeDD.exe version`、请求和结果、
`mod config <instance>`，以及开启 `trace=true` 时的 `logs\latest.log`（会显示输入，模拟的还是真实的）。
