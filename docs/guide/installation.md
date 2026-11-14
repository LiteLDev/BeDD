# Installation

## Install

From the [releases](https://github.com/LiteLDev/BeDD/releases), pick one:

| | |
|---|---|
| `bedd-windows-x64-setup.exe` | the installer: run it, accept the licence, done |
| `bedd-windows-x64.msi` | the same, for silent and managed installs: `msiexec /i bedd-windows-x64.msi /qn` |
| `bedd-windows-x64.zip` | no installer: unzip anywhere and run `BeDD.exe` from there |

The installer needs no administrator rights: it installs for you alone, to `%LOCALAPPDATA%\Programs\BeDD`, adds a
**BeDD** entry to the Start menu (it opens the [manager window](/guide/several-games#the-manager-window)) and puts the
folder on your `PATH`, so a new terminal finds `BeDD.exe`. The folder (and the zip) holds:

```
BeDD.exe                    the app: manager window and command line
mod\BeDD\                   the client mod (BeDD.dll, manifest.json)
mod\BeDD-server\            the server mod, for client + server runs
spec\bedd.asyncapi.yaml     the WebSocket protocol, for code generators
```

`BeDD.exe` is self-contained: it needs no .NET runtime and no PowerShell. Be.D.D. is not a lip package and is not
listed on LeviMC Mods.

### Upgrading

Run the newer installer. If the manager window is open, the installer closes it first, and with it the games it
shows. Your clones keep the mod they have until their next start:
`BeDD.exe launch` puts the mod of the installed `BeDD.exe` into a clone when it differs, and keeps the clone's
`config.json`. `launch --keep-mod` starts a clone with the mod it has.

### Uninstalling

Remove **BeDD** in Settings > Apps > Installed apps. That removes the app, the Start menu entry and the `PATH` entry;
your clones, worlds and logs in `%LOCALAPPDATA%\BeDD` stay. To remove those too, first remove each clone
(`BeDD.exe instance remove --slot N --yes`), then delete that folder. Your LeviLauncher instance is never touched.

## Requirements

- Windows 10 or 11, x64.
- [LeviLauncher](https://docs.levimc.org/LeviLauncher/) **1.1** or newer (the first version with a command line),
  with a Minecraft Bedrock Edition **1.26.60** (GDK, release) instance that has LeviLamina **26.60.x** (client).

If you do not have that instance yet, LeviLauncher's own command line makes it:

```powershell
LeviLauncher.exe account login              # a Microsoft account that owns Minecraft
LeviLauncher.exe install 1.26.60 --loader   # the game, with LeviLamina
```

Only the test PC's game needs Be.D.D.; servers and players do not. See [Setting up a new PC](/guide/new-pc) for how
Be.D.D. finds that instance.

## Try it by hand

```powershell
BeDD.exe launch --slot 1 --detach           # clones the instance (1.26.60.05-1), puts the mod in, starts it
```

`launch` makes the clone and installs the mod when they are missing; `BeDD.exe instance clone --slot 1` and
`BeDD.exe mod install 1.26.60.05-1` do those steps alone (`mod install` also writes [config](/guide/configuration)
keys).

The game shows up in the [manager window](/guide/several-games#the-manager-window), which `launch` opens when it is
not running yet. Then [drive it](/guide/usage). The base instance you play is never touched: Be.D.D. installs its mod
only into its own clones, so a normally started game stays as it was.

## Let the harness fetch it

A harness can fetch Be.D.D. itself instead of expecting an installed copy. `curl` and `tar` ship with Windows 10 and
11, so this works from any shell or language; `BEDD_DIR` points at an unpacked copy when you have one:

```powershell
curl.exe -L -o "$env:TEMP\bedd.zip" https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-windows-x64.zip
mkdir "$env:LOCALAPPDATA\BeDD\app" -Force; tar.exe -xf "$env:TEMP\bedd.zip" -C "$env:LOCALAPPDATA\BeDD\app"
```

Pin a version in CI (`releases/download/v0.4.0/...`) so a new release does not change a run under you.

## Build it yourself

See [Building and contributing](/reference/building).

## Next

On a PC without LeviLauncher or a game instance yet, read [Setting up a new PC](/guide/new-pc). Otherwise go on to
[Driving the game](/guide/usage).
