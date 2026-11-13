# Setting up a new PC

Be.D.D. needs one thing prepared: a LeviLauncher instance of Minecraft 1.26.60 with LeviLamina. Everything else it
does itself, in its own folder.

## The game

Signing in and installing the game are LeviLauncher's job (a script cannot buy Minecraft for you):

```powershell
LeviLauncher.exe account login              # once per PC: a Microsoft account that owns Minecraft
LeviLauncher.exe install 1.26.60 --loader   # the GDK release, with LeviLamina 26.60.x
LeviLauncher.exe instance list              # check: 1.26.60.xx, loader 26.60.x
```

You need LeviLauncher **1.1** or newer: older ones have no command line. When `BeDD.exe` finds no LeviLauncher at
all, it downloads the latest release to `%LOCALAPPDATA%\BeDD\tools` and tells you to run the two steps above with it.

## Which instance

Be.D.D. asks LeviLauncher for its instances (`LeviLauncher.exe instance list --json`) and uses the one of 1.26.60
with LeviLamina as the **base**. It only reads it: Be.D.D. never writes into a LeviLauncher instance, and never
starts one through the launcher.

```powershell
BeDD.exe instance list --all                # BeDD's clones, and the LeviLauncher instances they come from
BeDD.exe instance show 1.26.60.05-1         # one of them: folder, base, game and loader version, mod
```

To point it elsewhere:

| | |
|---|---|
| `BEDD_LEVILAUNCHER` | the `LeviLauncher.exe` to ask (default: the installed one, else the one in `tools`) |
| `BEDD_INSTANCE` or `--base <name>` | the base instance, by name, when there are several of 1.26.60 |

## Clones

Every game Be.D.D. starts is a **clone** of the base, slot 1 included: `<gameVersion>-<slot>`, e.g. `1.26.60.05-2`.
A clone has its own `mods\`, logs, worlds and options, and is made in seconds because the game files are
hardlinked to the base's.

```powershell
BeDD.exe instance clone --slot 2            # makes 1.26.60.05-2, or brings it up to date
BeDD.exe instance refresh 1.26.60.05-2      # re-links it after the base changed (an update, a new mod)
BeDD.exe instance path 1.26.60.05-2         # its folder, for scripts
BeDD.exe instance remove 1.26.60.05-2       # deletes it (never the base)
```

`BeDD.exe launch --slot N` makes the clone when it is missing, and refreshes it when the base has changed since.

::: tip Same volume
Hardlinks work only within one volume. When the base (under `%LOCALAPPDATA%\LeviLauncher\Instances`) and
Be.D.D.'s `instances` folder are on different drives, every clone is a full copy (several GB, and minutes instead of
seconds). Set `BEDD_INSTANCES_DIR` to a folder on the base's volume to avoid that; `instance show` tells
which.
:::

## Folders

Everything Be.D.D. keeps is under `%LOCALAPPDATA%\BeDD`:

| Folder | |
|---|---|
| `instances` | the clones (`BEDD_INSTANCES_DIR` moves just this one) |
| `run` | one `<pid>.json` per running game or server: its WebSocket URL and token ([Driving the game](/guide/usage#which-game)) |
| `locks` | the slot and signaling locks, held by the running games |
| `servers` | Bedrock Dedicated Servers for [client + server runs](/guide/modes) (`BEDD_SERVERS_DIR`) |
| `tools` | what Be.D.D. downloaded: LeviLauncher when none was installed, lip for servers |
| `logs` | the app's own logs (`manager.log`, …) |

`BEDD_HOME` moves the whole tree, e.g. to a fast data drive on a CI machine.

## Next

[Driving the game](/guide/usage).
