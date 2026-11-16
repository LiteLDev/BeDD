# Installs and clones

Installing Be.D.D. and setting up the first game on a PC: the tutorial
[Drive the game client with Be.D.D.](https://levimc.org/tutorials/automate/bedd/). This page is what comes after:
upgrades, CI, instances, clones and folders.

## Upgrading

Run the newer installer; it closes an open manager window and its games. Each `launch` then installs the clone's mod
when missing, or replaces it (keeping `config.json`) when it differs from its source:

- default: `mod\BeDD` next to `BeDD.exe`, else `bin\BeDD` of the checkout containing the current folder;
- after `mod install <instance> --from <dir>`: that folder (pinned in `mods\BeDD\source.pin`); if it has no build any
  more, `launch` warns and leaves the mod. `mod install` without `--from` ends the pin.

Not replaced: with `launch --keep-mod`, or while a game of the clone runs.

## Uninstalling

Settings > Apps > Installed apps > **BeDD** removes the app, Start menu and `PATH` entries. To also remove
`%LOCALAPPDATA%\BeDD` (clones, worlds, logs): `BeDD.exe instance remove --slot N --yes` each clone, then delete it.

## Let the harness fetch it

`BEDD_DIR` points at an unpacked copy. Pin a version in CI (`releases/download/v0.4.0/...`).

```powershell
curl.exe -L -o "$env:TEMP\bedd.zip" https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-windows-x64.zip
mkdir "$env:LOCALAPPDATA\BeDD\app" -Force; tar.exe -xf "$env:TEMP\bedd.zip" -C "$env:LOCALAPPDATA\BeDD\app"
```

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
