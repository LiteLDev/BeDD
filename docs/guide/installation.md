# Installation

## Download

Download `bedd-windows-x64.zip` from the [releases](https://github.com/LiteLDev/BeDD/releases) and unzip it
anywhere. It holds:

```
BeDD.exe                    the app: manager window and command line
mod\BeDD\                   the client mod (BeDD.dll, manifest.json)
mod\BeDD-server\            the server mod, for client + server runs
spec\bedd.asyncapi.yaml     the WebSocket protocol, for code generators
```

`BeDD.exe` is self-contained: it needs no .NET runtime, no PowerShell and no installer. Put its folder on `PATH`
if you like; the examples here call it as `BeDD.exe`.

Be.D.D. is not a lip package and is not listed on LeviMC Mods.

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
BeDD.exe instance clone --slot 1            # your clone of the instance, e.g. 1.26.60.05-1
BeDD.exe mod install 1.26.60.05-1           # puts mod\BeDD into it and writes its config
BeDD.exe launch --slot 1 --detach           # starts it in the background: pid, url, token
```

The game shows up in the [manager window](/guide/several-games#the-manager-window), which `launch` opens when it is
not running yet. Then [drive it](/guide/usage). The base instance you play is never touched: Be.D.D. installs its mod
only into its own clones, so a normally started game stays as it was.

## Let the harness fetch it

A harness can fetch Be.D.D. itself instead of expecting a copy somewhere. `curl` and `tar` ship with Windows 10 and
11, so this works from any shell or language; `BEDD_DIR` points at an unpacked copy when you have one:

```powershell
curl.exe -L -o "$env:TEMP\bedd.zip" https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-windows-x64.zip
mkdir "$env:LOCALAPPDATA\BeDD\app" -Force; tar.exe -xf "$env:TEMP\bedd.zip" -C "$env:LOCALAPPDATA\BeDD\app"
```

Pin a version in CI (`releases/download/v2.0.0/...`) so a new release does not change a run under you.

## Build it yourself

See [Building and contributing](/reference/building).

## Next

On a PC without LeviLauncher or a game instance yet, read [Setting up a new PC](/guide/new-pc). Otherwise go on to
[Driving the game](/guide/usage).
