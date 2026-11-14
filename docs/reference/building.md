# Building and contributing

## Building

Windows x64 plus (`winget install -e --id <id>`; LLVM, Build Tools need admin; reopen the terminal):

| Tool | id |
|---|---|
| xmake | `Xmake-io.Xmake` |
| LLVM `clang-cl` (mod toolchain) | `LLVM.LLVM` |
| Build Tools 2022/2026, C++ (MSVC 14.4x+) | `Microsoft.VisualStudio.2022.BuildTools` / `Microsoft.VisualStudio.BuildTools` + `--override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"` |
| .NET 10 SDK | `Microsoft.DotNet.SDK.10` |
| Node.js 24+ | `OpenJS.NodeJS.LTS` |

| Build | Commands | Output |
|---|---|---|
| Mod | `$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"`<br>`xmake f -P . -y -m release -p windows --target_type=client` (`-m debug`)<br>`xmake -P . -y`<br>`git checkout -- xmake-requires.lock` | `bin\BeDD\` |
| Server mod | `git checkout -- xmake-requires.lock`<br>`xmake f -P . -y -m release -p windows --target_type=server`<br>`xmake -P . -y`<br>`git checkout -- xmake-requires.lock` | `bin\BeDD-server\` → server `mods\BeDD-server\` (config `mods\BeDD-server\config\config.json`; [ops](/reference/websocket#server-target)) |
| App | `$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"` (vswhere for AOT)<br>`dotnet publish app/BeDD/BeDD.csproj -c Release -o bin\app`<br>`dotnet test app/BeDD.slnx` (no vswhere) | `bin\app\BeDD.exe` |
| Installers | `dotnet build packaging/wix/Bundle/BeDD.Bundle.wixproj -c Release -p:Platform=x64` | `packaging\wix\Package\bin\x64\Release\bedd.msi`, `packaging\wix\Bundle\bin\x64\Release\bedd-setup.exe` |
| Docs | `cd docs; npm ci; npm run build` | <https://docs.levimc.org/BeDD/> (CI: `docs.yml`) |

- `-P .`: else xmake may build a parent checkout (from a worktree).
- Version from `git describe`: needs a commit and tags (`git fetch --tags` when shallow).
- `xmake-requires.lock` pins both targets; xmake rewrites it with one: restore it after each build. New pin: configure
  both targets, commit the union.
- Server mod: WebSocket, loopback network, `src/ServerTimeScale.cpp` (list in `xmake.lua`).
- Installers (WiX 7 from NuGet, `AcceptEula` in `packaging/wix/Directory.Build.props`): per-user MSI + `setup.exe`
  (English/Chinese UI) from `bin\release\BeDD` (made by `.github/actions/stage` from `bin\app`, `bin\BeDD`,
  `bin\BeDD-server`, the spec); version from `app/BeDD/BeDD.csproj` or `-p:ProductVersion=`. Tests:
  `tests/installer/installer.test.mjs` (current user; refuses where BeDD is installed; CI).

## Dev loop

`bin\app\BeDD.exe dev`: clones slot 9 (`--slot`), installs `bin\BeDD` (`--from <dir>`), launches (label `dev`,
`--label`), and logs foreground and cursor-clip changes to `tools\out\desktop-watch.log` (`--watch-log`). A line ending
`MINECRAFT HAS THE FOREGROUND` (also warned): a game took focus. The clip must stay `full`.

Ctrl+C or `dev.stop` (log dir): close the game (unless `--keep`) and exit; `dev.restart`: reinstall and relaunch;
dev exits with the game. `--watch-only`: watch only.

Closes only its clone's games; lock `%LOCALAPPDATA%\BeDD\locks\dev-<slot>.lock`; blocks the shell.

## Documentation and showcase media

`docs/public/showcase/`: gallery and README media, made by `tools/showcase/record.mjs [--keep-open] [--dry-run]` and
`encode.mjs` ([details](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md#docs-and-showcase)).

## Source layout

Every source file and what it holds: [CONTRIBUTING.md](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md#source-layout).

Code rules (SDK headers, `MCAPI` only), pull requests, license: [CONTRIBUTING.md](https://github.com/LiteLDev/BeDD/blob/main/CONTRIBUTING.md).
