<#
.SYNOPSIS
  Build BeDD into bin\BeDD (or the server mod into bin\BeDD-server), on a PC that may not have the
  build tools yet.

.DESCRIPTION
  Finds xmake, LLVM (clang-cl) and the Visual Studio C++ build tools, installs whatever is missing
  with winget (LLVM and the build tools ask for administrator rights), puts LLVM on PATH for this
  build only and runs xmake. Works from Windows PowerShell and pwsh, in any terminal (no Developer
  Command Prompt needed).

.EXAMPLE
  pwsh -NoProfile -File tools/build.ps1

.EXAMPLE
  pwsh -NoProfile -File tools/build.ps1 -TargetType server
  The mod for a Bedrock Dedicated Server (the client+server mode), into bin\BeDD-server.
#>
param(
    [ValidateSet('release', 'debug')][string]$Mode = 'release',
    [ValidateSet('client', 'server')][string]$TargetType = 'client'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot/..").Path

function Write-Build([string]$m) { Write-Host "[build] $m" }

function Install-WingetPackage([string]$Id, [string[]]$Extra = @()) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw "$Id is missing and winget is not available: install it by hand (see CONTRIBUTING.md)" }
    Write-Build "installing $Id (winget)"
    winget install --id $Id -e --accept-source-agreements --accept-package-agreements @Extra | Out-Host
    if ($LASTEXITCODE) { throw "winget install $Id failed (exit code $LASTEXITCODE)" }
    # What the installer added to PATH, without a new terminal.
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH', 'User') + ';' + $env:PATH
}

function Find-Tool([string]$Command, [string[]]$Paths) {
    $c = Get-Command $Command -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    foreach ($p in $Paths) { if ($p -and (Test-Path $p)) { return $p } }
    return $null
}

# The MSVC headers, libraries and linker clang-cl builds against.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$findVs = { if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } }
if (-not (& $findVs)) {
    Install-WingetPackage Microsoft.VisualStudio.2022.BuildTools @('--override', '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    if (-not (& $findVs)) { throw 'The Visual Studio C++ build tools are still missing' }
}

$llvmBin = Join-Path $env:ProgramFiles 'LLVM\bin'
$clang = Find-Tool clang-cl @((Join-Path $llvmBin 'clang-cl.exe'))
if (-not $clang) {
    Install-WingetPackage LLVM.LLVM
    $clang = Find-Tool clang-cl @((Join-Path $llvmBin 'clang-cl.exe'))
    if (-not $clang) { throw 'clang-cl is still missing after installing LLVM' }
}
$env:PATH = (Split-Path $clang) + ';' + $env:PATH

$xmakePaths = @((Join-Path $env:ProgramFiles 'xmake\xmake.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\xmake\xmake.exe'))
$xmake = Find-Tool xmake $xmakePaths
if (-not $xmake) {
    Install-WingetPackage Xmake-io.Xmake
    $xmake = Find-Tool xmake $xmakePaths
    if (-not $xmake) { throw 'xmake is still missing after installing it' }
}

# The mod packer takes the version from the latest tag.
$ErrorActionPreference = 'Continue' # git's stderr would stop Windows PowerShell
$null = git -C $Root describe --tags 2>&1
if ($LASTEXITCODE) { Write-Build 'no git tag here (shallow clone?): fetching tags for the version'; git -C $Root fetch --tags --quiet }
$ErrorActionPreference = 'Stop'

# xmake-requires.lock pins the packages of both targets, but xmake rewrites it with the configured
# target's pins only: it is put back as it was once the build is done.
$lockFile = Join-Path $Root 'xmake-requires.lock'
$lock = if (Test-Path $lockFile) { [IO.File]::ReadAllBytes($lockFile) }

# -P: xmake would otherwise build an enclosing project instead (a worktree inside another checkout).
Push-Location $Root
try {
    & $xmake repo -u -P $Root
    if ($LASTEXITCODE) { throw "xmake repo -u failed (exit code $LASTEXITCODE)" }
    & $xmake f -P $Root -a x64 -m $Mode -p windows --target_type=$TargetType -y
    if ($LASTEXITCODE) { throw "xmake f failed (exit code $LASTEXITCODE)" }
    & $xmake -P $Root -y
    if ($LASTEXITCODE) { throw "xmake failed (exit code $LASTEXITCODE)" }
} finally {
    if ($lock) { [IO.File]::WriteAllBytes($lockFile, $lock) }
    Pop-Location
}
Write-Build "done: $(Join-Path $Root $(if ($TargetType -eq 'server') { 'bin\BeDD-server' } else { 'bin\BeDD' }))"
