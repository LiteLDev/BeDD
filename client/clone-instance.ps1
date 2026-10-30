<#
.SYNOPSIS
  Create (or refresh) an extra game instance for parallel headless e2e runs.

.DESCRIPTION
  Clones the LeviLauncher instance versions\<Source> into versions\<Source>-hi<Slot> cheaply:
  game files are hard links, the large read-only folders (data, Installers) are junctions, and
  only the per-instance state is copied: "Minecraft Bedrock" (isolated game data: worlds,
  options.txt, sign-in) and mods\LeviLamina. logs\ starts empty; other mods are not copied (the
  harness deploys what it needs).

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
    [string]$Source = '1.26.51.01',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'

$versions = Join-Path $env:APPDATA 'levilauncher.exe\versions'
$src = Join-Path $versions $Source
$name = "$Source-hi$Slot"
$dst = Join-Path $versions $name
if (-not (Test-Path (Join-Path $src 'Minecraft.Windows.exe'))) { throw "No instance at $src" }

if (Test-Path $dst) {
    if (-not $Force) { Write-Host "[clone] $dst exists (pass -Force to rebuild it)"; return }
    # Junctions are removed without touching their targets; hard links only drop a name.
    Get-ChildItem $dst -Force | Where-Object { $_.LinkType -eq 'Junction' } | ForEach-Object { $_.Delete() }
    Remove-Item -Recurse -Force $dst
}
New-Item -ItemType Directory $dst | Out-Null

$junctions = @('data', 'Installers')
$copies    = @('Minecraft Bedrock')
$skip      = @('logs', 'mods', 'mods-disabled')

foreach ($item in Get-ChildItem $src -Force) {
    $target = Join-Path $dst $item.Name
    if ($item.PSIsContainer) {
        if ($item.Name -in $skip -or $item.Name -like 'mods.*') { continue }
        if ($item.Name -in $junctions) { New-Item -ItemType Junction -Path $target -Target $item.FullName | Out-Null }
        elseif ($item.Name -in $copies) { Copy-Item -Recurse $item.FullName $target }
        else { Copy-Item -Recurse $item.FullName $target }
    } elseif ($item.Name -eq 'version.json') {
        $v = Get-Content $item.FullName -Raw | ConvertFrom-Json
        $v.name = $name
        $v | ConvertTo-Json | Set-Content -Encoding utf8 $target
    } else {
        New-Item -ItemType HardLink -Path $target -Target $item.FullName | Out-Null
    }
}
New-Item -ItemType Directory (Join-Path $dst 'logs') | Out-Null
New-Item -ItemType Directory (Join-Path $dst 'mods') | Out-Null
foreach ($m in 'LeviLamina', 'CrashLogger') {
    $p = Join-Path $src "mods\$m"
    if (Test-Path $p) { Copy-Item -Recurse $p (Join-Path $dst "mods\$m") }
}
Write-Host "[clone] $name ready at $dst"
