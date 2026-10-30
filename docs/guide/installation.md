# Installation

## Download

Download `bedd-client-windows-x64.zip` from the [releases](https://github.com/LiteLDev/BeDD/releases) and unzip it
anywhere. It holds a `BeDD` folder with the module (`BeDD.dll`, `manifest.json`), the PowerShell client and the
scripts.

- **For tests**, harnesses copy the module into the game instance's `mods\` folder at the start of a run and remove
  it afterwards ([Plugging into a harness](/guide/harness)).
- **To try it by hand**, unzip it into the instance's `mods\` folder, so that you get `mods\BeDD\BeDD.dll`, and
  write a [config](/guide/configuration) that turns background mode on. Remove the folder again when you are done:
  a leftover copy with background mode on makes a normally started game ignore your mouse and keyboard.

Be.D.D. is not a lip package and is not listed on LeviMC Mods.

## Requirements

Minecraft Bedrock Edition **1.26.51** (GDK) with LeviLamina **26.51.x** (client), in a game instance installed with
LeviLauncher. Tests start that instance with `Start-BeDDGame.ps1` rather than through the launcher
([Several games at once](/guide/several-games)). Only the test PC's game needs it; servers and players do not.

The manager window needs PowerShell 7 (`pwsh`).

## Let the harness fetch it

A harness can fetch Be.D.D. itself instead of expecting a checkout somewhere. `BEDD_DIR` points at an unpacked
copy when you have one:

```powershell
$bedd = if ($env:BEDD_DIR) { $env:BEDD_DIR } else { Join-Path $env:LOCALAPPDATA 'BeDD\client' }
if (-not (Test-Path "$bedd\harness-shim.ps1")) {
    $zip = Join-Path $env:TEMP 'bedd.zip'
    Invoke-WebRequest https://github.com/LiteLDev/BeDD/releases/latest/download/bedd-client-windows-x64.zip -OutFile $zip
    Expand-Archive $zip (Split-Path $bedd) -Force; Rename-Item (Join-Path (Split-Path $bedd) 'BeDD') $bedd
}
```

## Build it yourself

See [Building and contributing](/reference/building).

## Next

On a PC without LeviLauncher, LeviLamina or a game instance yet, read [Setting up a new PC](/guide/new-pc).
Otherwise go on to [Driving the game](/guide/usage).
