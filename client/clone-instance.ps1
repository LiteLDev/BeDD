<#
.SYNOPSIS
  Create (or refresh) an extra game instance for parallel headless e2e runs.

.DESCRIPTION
  Clones the LeviLauncher instance versions\<Source> into versions\<Source>-hi<Slot> cheaply:
  game files are hard links, the large read-only folders (data, Installers) are junctions, and
  only the per-instance state is copied: "Minecraft Bedrock" (isolated game data: worlds,
  options.txt, sign-in) and mods\LeviLamina. logs\ starts empty; other mods are not copied (the
  harness deploys what it needs). An existing clone is brought up to date with its source (game
  files the source got since, e.g. from a LeviLamina update); -Force rebuilds it from scratch.

  Without -Source it clones the Minecraft 1.26.51 instance BeDD finds (BeDDSetup.psm1), and sets
  the source up first if this PC lacks something (LeviLamina, LeviLauncher's loader, the game
  itself). Start-BeDDGame.ps1 makes a missing clone on its own, so harnesses need not call this.

  Every instance is a separate folder with its own mods\, logs\latest.log and game data, so
  several games can run at the same time, each driven through its own BeDD pipe.

  Start a clone only as Minecraft.Windows.exe (Start-BeDDGame.ps1): LeviLauncher.dll's data
  isolation keys on that file name. Under another name the game silently uses the default
  %APPDATA%\Minecraft Bedrock (your normal Minecraft profile) instead of the clone's data.

  Every harness on the machine must then only touch its own instance's game (match the process
  path, not just the name "Minecraft.Windows"): a harness that focuses, feeds or closes every
  Minecraft.Windows process breaks the other instances' runs.

.EXAMPLE
  pwsh -NoProfile -File bin/BeDD/clone-instance.ps1 -Slot 2
#>
param(
    [Parameter(Mandatory)][ValidateRange(1, 32)][int]$Slot,
    [string]$Source,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'BeDDSetup.psm1') -DisableNameChecking

$base = Initialize-BeDDInstance -Name $Source
New-BeDDClone -Source $base.Name -Slot $Slot -Force:$Force
