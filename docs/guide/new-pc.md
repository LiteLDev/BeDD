# Setting up a new PC

## The game

```powershell
LeviLauncher.exe account login              # once per PC: a Microsoft account that owns Minecraft
LeviLauncher.exe install 1.26.60 --loader   # the GDK release with LeviLamina 26.60.x
LeviLauncher.exe instance list              # check: 1.26.60.xx, loader 26.60.x
```

LeviLauncher 1.1+ is required (first with a CLI). If none is installed, `BeDD.exe` downloads the latest to
`%LOCALAPPDATA%\BeDD\tools` and asks you to run the steps above with it.

## Which instance

`BeDD.exe` takes the 1.26.60 LeviLamina instance from `LeviLauncher.exe instance list --json` as the **base**. It
never writes into it or starts it.

| Override | |
|---|---|
| `BEDD_LEVILAUNCHER` | the `LeviLauncher.exe` to ask (default: installed, else the one in `tools`) |
| `BEDD_INSTANCE` / `--base <name>` | the base, when several are 1.26.60 |

## Clones

Every game runs in a clone `<gameVersion>-<slot>` (e.g. `1.26.60.05-2`; slot 1 too) with its own `mods\`, logs,
worlds and options; game files are hardlinked to the base.

```powershell
BeDD.exe instance list --all                # clones and their LeviLauncher bases
BeDD.exe instance show 1.26.60.05-2         # folder, base, game/loader version, mod, hardlinked or copied
BeDD.exe instance clone --slot 2            # make or update
BeDD.exe instance refresh 1.26.60.05-2      # re-link after the base changed
BeDD.exe instance path 1.26.60.05-2         # its folder
BeDD.exe instance remove 1.26.60.05-2       # delete (never the base)
```

`launch --slot N` clones when missing and refreshes when the base changed. Hardlinks need one volume: if
`%LOCALAPPDATA%\LeviLauncher\Instances` and `instances` are on different drives, each clone is a full copy (GB,
minutes); set `BEDD_INSTANCES_DIR` on the base's volume.

## Folders

`%LOCALAPPDATA%\BeDD` (`BEDD_HOME` moves it):

| | |
|---|---|
| `instances` | clones (`BEDD_INSTANCES_DIR`) |
| `run` | `<pid>.json` per running game or server: URL and token ([Which game](/guide/usage#which-game)) |
| `locks` | slot and signaling locks |
| `servers` | dedicated servers ([client + server](/guide/modes); `BEDD_SERVERS_DIR`) |
| `tools` | downloaded LeviLauncher, lip |
| `logs` | the app's logs (`manager.log`, …) |
