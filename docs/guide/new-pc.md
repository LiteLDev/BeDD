# Setting up a new PC

Nothing needs preparing by hand except the game itself. `Initialize-BeDD.ps1` (also run by `Start-BeDDGame.ps1`
and `clone-instance.ps1`) finds the instance and sets up whatever the PC lacks, each step only once:

| Missing | What happens |
|---|---|
| LeviLauncher | the installed one is used; otherwise the latest release is downloaded to `%LOCALAPPDATA%\BeDD\tools` |
| the game | LeviLauncher opens and the script waits while you sign in with a Microsoft account that owns Minecraft and install 1.26.51 (a script cannot do this step; with `CI` set it fails with these instructions instead) |
| LeviLamina | installed into the instance with [lip](https://docs.levimc.org/lip/), which is downloaded with the .NET runtime it needs (per user, no admin) |
| LeviLauncher's loader in the game exe | the instance is started once through LeviLauncher, which adds it, and the game is closed again. Without the loader the game would use your normal Minecraft data instead of the instance's |
| a clone (`-Slot 2` and up) | made from the base instance, and refreshed when the base instance has changed since |

```powershell
$i = & .\Initialize-BeDD.ps1 -Slot 2    # Name, Dir, ModsDir, PipeName of versions\<base>-hi2
```

## Which instance

Be.D.D. finds LeviLauncher's data in `%APPDATA%\levilauncher.exe`, follows its "data folder" setting
(`base_root`), and picks the instance of 1.26.51 there (one with LeviLamina first).

To point it elsewhere, set these environment variables:

| Variable | |
|---|---|
| `BEDD_INSTANCE` | the instance name |
| `BEDD_VERSIONS_DIR` | LeviLauncher's `versions` folder |
| `BEDD_LAUNCHER_DATA` | LeviLauncher's data folder |
| `BEDD_LEVILAUNCHER` | `LeviLauncher.exe` |
| `BEDD_LIP` | `lip.exe` |

## From PowerShell

`BeDDSetup.psm1` (imported by the harness shim) has the same steps as functions:

- `Initialize-BeDDInstance [-Name n] [-Slot N]` returns the instance to use (`Name`, `Dir`, `ModsDir`, `PipeName`)
  after setting up what is missing.
- `Get-BeDDInstance` describes an instance without changing anything.
- `New-BeDDClone -Source <base> -Slot N` makes a clone, also for slots above 32.
- `Get-LeviLauncherData`, `Get-BeDDVersionsDir` give the folders it found.

## Next

[Driving the game](/guide/usage).
