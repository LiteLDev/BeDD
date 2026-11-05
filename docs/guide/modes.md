# Client and server modes

Be.D.D. debugs a client mod in one of three setups:

| Mode | What runs | Use it for |
|---|---|---|
| **Client only** | a game with the BeDD module, in a singleplayer or LAN world | most client mod tests |
| **Client + server** | a game with BeDD and a Bedrock Dedicated Server (BDS) with BeDD's server module, on the same PC | client behaviour against a real server, both sides scripted; the server's game speed leads the client's |
| **Client → remote** | a game with BeDD joined to a server elsewhere | a server you do not run (BeDD is only in the client) |

The client side is the same in all three: [Driving the game](/guide/usage), [Several games at once](/guide/several-games).
This page is about the server.

## Client + server

`BeDDSetup.psm1` sets up a server and `Start-BeDDServer.ps1` runs it in the background, both next to the module in
`bin\BeDD`:

```powershell
Import-Module .\BeDDSetup.psm1
Initialize-BeDDServer -Name s1 -ModuleDir ..\BeDD-server    # %LOCALAPPDATA%\BeDD\servers\s1, made ready
$s = & .\Start-BeDDServer.ps1 -Name s1                       # up, with BeDD's pipe; $s.Port, $s.PipeName
Connect-BeDD -PipeName $s.PipeName                           # the server's control pipe (BeDD.psm1)
# ... start a game (Start-BeDDGame.ps1) and join 127.0.0.1:$($s.Port) from it ...
& .\Start-BeDDServer.ps1 -Name s1 -Stop                      # saves the world and exits
```

### Setting up a server

`Initialize-BeDDServer` installs LeviLamina's server package with lip into an empty folder; that package brings the
official BDS of Minecraft 1.26.60 with it (downloaded from minecraft.net, about 100 MB). Like the game setup, it
skips whatever is already done, runs one setup at a time on the PC and never asks anything, so harnesses and CI can
call it before every run. A server's mods live in `mods\`, so the module goes to `mods\BeDD-server` (LeviLamina loads a mod only from the folder named after it).

| Parameter | |
|---|---|
| `-Name` | the server, a folder under `%LOCALAPPDATA%\BeDD\servers` (`BEDD_SERVERS_DIR`); default `server` |
| `-Port`, `-PortV6` | default: the server's ports from before, else the first free pair of 19140/19141, 19142/19143, …, so servers never meet a game's 19132 or each other |
| `-Transport` | default `raknet`: `Join-BeDDServer` joins over RakNet. BDS 1.26.51 logs a "TRANSPORT TYPE ERROR" (players cannot connect without NetherNet); ignore it, they can. A join over `nethernet` to a server kept on loopback hangs |
| `-ServerIp` | default `127.0.0.1`: only games on this PC join; `''` listens on every interface |
| `-OnlineMode` | default `$false`: games join without Xbox authentication |
| `-LevelName`, `-Gamemode`, `-Difficulty`, `-LevelSeed`, `-MaxPlayers`, `-AllowCheats` | `server.properties` (defaults `Bedrock level`, `creative`, `peaceful`, random, 10, `$true`) |
| `-Properties @{ 'view-distance' = 16 }` | any other `server.properties` key |
| `-ModuleDir` | the BeDD server module (`bin\BeDD-server`), copied to `mods\BeDD-server` when it changed. `Start-BeDDServer.ps1` starts no server without it ([no firewall prompts](#no-firewall-prompts)) |
| `-PipeName` | BeDD's control pipe in the server, default `BeDD-Server-<Name>` |
| `-TimeScale` | config `timeScale`: the game speed the server starts with |
| `-Trace`, `-Config @{ … }` | config `trace`, and any other key of the module's `config\config.json` |

It returns the server: `Dir`, `Port`, `PortV6`, `PipeName`, `LogPath` and `Problems` (`bedd` while it has no BeDD
module). `Get-BeDDServer -Name s1` returns the same without changing anything.

### Starting and stopping

`Start-BeDDServer.ps1 -Name s1` starts `bedrock_server_mod.exe` (the BDS with LeviLamina) with no window at all:
nothing shows up on the desktop or takes the focus. Everything the server prints goes to `logs\console.log` in its folder. It waits until the server is
up and, with the module, until BeDD's pipe exists, and returns `Process`, `PipeName` (`$null` without the module),
`Port`, `PortV6`, `Dir` and `LogPath`. A server that already runs is returned as it is.

| Parameter | |
|---|---|
| `-Name`, `-ServersDir` | which server |
| `-Command '<line>'` | type a line into the server console, e.g. `-Command 'time set day'` |
| `-Stop` | send `stop` (the server saves the world and exits), and kill it after `-StopTimeoutSeconds` (30) |
| `-TimeoutSeconds` | how long to wait for it to start (180) |
| `-AllowFirewallPrompt` | start it without BeDD's loopback binding (see below) |

### No firewall prompts

A BDS listens on every network interface, and the first time a program does that Windows Firewall asks on the desktop
whether to allow it, once per server folder, which would stop an unattended run. BeDD's server module binds every
socket of the server to the loopback address instead (config `loopbackOnly`, default on, as in the client), so Windows
never asks, and only games on this PC can join. `Initialize-BeDDServer` also sets `server-ip=127.0.0.1` and turns LAN
visibility off, which would otherwise bind Bedrock's default ports as well.

So `Start-BeDDServer.ps1` refuses to start a server that has no BeDD server module in `mods\BeDD-server`, has
another module there, or has `"loopbackOnly": false` in its `config.json`, and says which. Should LeviLamina still
fail to load the module, it stops the server before the server opens its network. `-AllowFirewallPrompt` starts it
anyway, for a server that games on other PCs should join (with `-ServerIp ''`): then allow it when Windows asks.

### Several servers

Each server has its own folder, ports and pipe, so runs in parallel each start their own: give each a `-Name` of its
own (e.g. the slot of its game) and leave `-Port` out.
