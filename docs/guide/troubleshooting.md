# Troubleshooting

| Symptom | Cause → fix |
|---|---|
| HTTP 401 | bad or old token (new per start) → take `url`+`token` together from `ps --json` |
| HTTP 403 | `Origin` header → drop it or list it in `allowOrigins` |
| connection refused | not up yet or exited → `launch` without `--detach` |
| too many connections | over `wsMaxClients` (8) → close some |
| game closes after ~30 s | no manager (see `latest.log`) → `manager status`, `logs\manager.log`, drop `--no-manager`; CI: config `manager=false` |
| clone takes minutes and GB | copied across volumes → `BEDD_INSTANCES_DIR` on the base's drive, re-clone ([Clones](/guide/new-pc#clones)) |
| LeviLauncher not found | missing or < 1.1 → update, or `BEDD_LEVILAUNCHER` |
| instance not found | → `LeviLauncher.exe install 1.26.60 --loader`; several: `BEDD_INSTANCE`; see `instance list --all` |
| no WebSocket, mod not loaded | not 1.26.60 + LeviLamina 26.60.x (`latest.log`) → `LeviLauncher.exe loader install`, `instance refresh` |
| clone has an old or wrong BeDD mod | `--keep-mod`, running game, or `--from` pin → `mod install <instance>` ([Upgrading](/guide/installation#upgrading)) |
| "An error has occurred" ("Blaze") | shared sign-in → `world singleplayer`; online: `--exclusive-signaling` |
| game uses my normal worlds and options | not started by `launch`, or base not from LeviLauncher 1.1+ |
| firewall prompt per clone | `loopbackOnly` off → on; non-BeDD servers ask once (Cancel) |
| console window flashes at start | `CrashLogger.exe` → `launch` fixes it ([Configuration](/guide/configuration)) |
| closed someone else's game | by process name → by instance; one slot per run |
| `button` did nothing after hang-up | endless `down` released → `click`/`holdTicks`/`releaseOn` ([Several connections](/guide/usage#several-connections)) |
| `look` times out, game ops idle | world paused (no ticks) → `key ESC` |
| `ui` `stale_ref` / `occluded` / `not_ready` / `unsupported` | → read `ui` again / `force:true` / retry / [simulated input](/guide/operation-modes#simulated-input) ([UI tree](/reference/ui)) |
| `code:"disabled"` | [feature key](/guide/configuration#feature-keys) `false`; `unknown op`: not in this build |

## Reporting a bug

[Open an issue](https://github.com/LiteLDev/BeDD/issues/new/choose) with `BeDD.exe version`, the request and result,
`mod config <instance>`, and `logs\latest.log` with `trace=true` (shows input, synthetic or real).
