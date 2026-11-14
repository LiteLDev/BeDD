# Installation

## Install

From the [releases](https://github.com/LiteLDev/BeDD/releases):

| | |
|---|---|
| `bedd-windows-x64-setup.exe` | installer |
| `bedd-windows-x64.msi` | silent/managed: `msiexec /i bedd-windows-x64.msi /qn` |
| `bedd-windows-x64.zip` | unzip anywhere, run `BeDD.exe` |

The installer needs no admin rights: per user, to `%LOCALAPPDATA%\Programs\BeDD`, with a **BeDD** Start menu entry
(the manager window) and the folder on `PATH`. Contents: `BeDD.exe`, `mod\BeDD\` (BeDD.dll, manifest.json),
`mod\BeDD-server\`, `spec\bedd.asyncapi.yaml`. `BeDD.exe` is self-contained.

```powershell
BeDD.exe launch --slot 1 --json    # clone 1.26.60.05-1, install the mod, start; prints url, token
```

### Upgrading

Run the newer installer; it closes an open manager window and its games. Each `launch` then installs the clone's mod
when missing, or replaces it (keeping `config.json`) when it differs from its source:

- default: `mod\BeDD` next to `BeDD.exe`, else `bin\BeDD` of the checkout containing the current folder;
- after `mod install <instance> --from <dir>`: that folder (pinned in `mods\BeDD\source.pin`); if it has no build any
  more, `launch` warns and leaves the mod. `mod install` without `--from` ends the pin.

Not replaced: with `launch --keep-mod`, or while a game of the clone runs.

### Uninstalling

Settings > Apps > Installed apps > **BeDD** removes the app, Start menu and `PATH` entries. To also remove
`%LOCALAPPDATA%\BeDD` (clones, worlds, logs): `BeDD.exe instance remove --slot N --yes` each clone, then delete it.

## Let the harness fetch it

`BEDD_DIR` points at an unpacked copy. Pin a version in CI (`releases/download/v0.4.0/...`).

```powershell
curl.exe -L -o "$env:TEMP\bedd.zip" https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-windows-x64.zip
mkdir "$env:LOCALAPPDATA\BeDD\app" -Force; tar.exe -xf "$env:TEMP\bedd.zip" -C "$env:LOCALAPPDATA\BeDD\app"
```

## Build it yourself

[Building and contributing](/reference/building).
