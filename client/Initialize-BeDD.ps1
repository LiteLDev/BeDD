<#
.SYNOPSIS
  Get this PC ready for headless games: find (or set up) the game instance BeDD runs.

.DESCRIPTION
  Finds the LeviLauncher instance of Minecraft 1.26.51 and sets up whatever this PC still lacks
  (BeDDSetup.psm1): LeviLauncher itself (downloaded from its GitHub releases), the game (LeviLauncher
  opens for you to sign in and install it: the one step a script cannot do), LeviLamina (installed
  with lip, which is downloaded with its .NET runtime), LeviLauncher's loader in the game exe, and
  the clone for -Slot 2 and up. Every step is skipped when already done, so it is cheap to call at
  the start of every run. Start-BeDDGame.ps1 and clone-instance.ps1 call the same code.

  Returns the instance: Name, Dir, ModsDir, Base, Slot, VersionsDir, PipeName (a pipe name of its
  own for Install-BeDDMod / Connect-BeDD).

.EXAMPLE
  pwsh -NoProfile -File bin/BeDD/Initialize-BeDD.ps1            # the base instance
  $i = & bin\BeDD\Initialize-BeDD.ps1 -Slot 2                  # its clone <base>-hi2
  Install-BeDDMod -ModsDir $i.ModsDir -PipeName $i.PipeName
#>
param(
    [string]$Instance,
    [int]$Slot = 1,
    [int]$TimeoutMinutes = 30
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'BeDDSetup.psm1') -DisableNameChecking

Initialize-BeDDInstance -Name $Instance -Slot $Slot -TimeoutMinutes $TimeoutMinutes
