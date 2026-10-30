# Finds the LeviLauncher game instance BeDD runs games in and, on a PC that lacks something, sets it
# up: Start-BeDDGame.ps1, clone-instance.ps1 and Initialize-BeDD.ps1 call Initialize-BeDDInstance,
# so a harness needs nothing prepared by hand beyond the one step only a person can do (installing
# the game in LeviLauncher, which needs a Microsoft account that owns Minecraft).
#
#   Import-Module <bin>\BeDD\BeDDSetup.psm1
#   $i = Initialize-BeDDInstance -Slot 2     # versions\<base>-hi2, made ready; $i.Dir, $i.ModsDir, $i.PipeName
#   Get-BeDDInstance                         # the same without changing anything; $i.Problems
#
# What it does when something is missing (each step is skipped when already done):
#   - no instance of Minecraft $GameVersion: finds LeviLauncher (or downloads it from its GitHub
#     releases), opens it and waits until the game is installed there;
#   - no LeviLamina in the instance: installs it with lip (downloaded too, with the .NET runtime it
#     needs, per user under %LOCALAPPDATA%);
#   - the game exe lacks LeviLauncher's loader (an instance never started from LeviLauncher; without
#     it the game would use your normal Minecraft data instead of the instance's): starts the
#     instance once through LeviLauncher, which adds it, and closes the game again;
#   - slot 2 and up: the clone versions\<base>-hi<N> (clone-instance.ps1), refreshed when the base
#     instance changed since.
#
# Overrides: BEDD_INSTANCE (base instance name), BEDD_VERSIONS_DIR (LeviLauncher's versions folder),
# BEDD_LAUNCHER_DATA (LeviLauncher's data folder), BEDD_LEVILAUNCHER (LeviLauncher.exe), BEDD_LIP (lip.exe).

# The game and LeviLamina this build of BeDD is made for (xmake.lua's levilamina package).
$script:GameVersion       = '1.26.51'
$script:LeviLaminaVersion = '26.51.7'
# Downloaded tools (LeviLauncher, lip, the .NET runtime for lip).
$script:ToolsDir = Join-Path $env:LOCALAPPDATA 'BeDD\tools'

function Write-Setup([string]$Message) { Write-Host "[BeDD] $Message" }

# LeviLauncher's own folder: %APPDATA%\<its exe name in lower case>, levilauncher.exe for every
# release (config.json, locks of the e2e harnesses).
function Get-LeviLauncherData {
    if ($env:BEDD_LAUNCHER_DATA) { return $env:BEDD_LAUNCHER_DATA }
    return Join-Path $env:APPDATA 'levilauncher.exe'
}

# Where LeviLauncher keeps its game instances: <data>\versions, or <base_root>\versions when its
# settings move the data elsewhere.
function Get-BeDDVersionsDir {
    if ($env:BEDD_VERSIONS_DIR) { return $env:BEDD_VERSIONS_DIR }
    $data = Get-LeviLauncherData
    try {
        $root = (Get-Content (Join-Path $data 'config.json') -Raw -ErrorAction Stop | ConvertFrom-Json).base_root
        if ($root -and $root.Trim()) { return Join-Path $root.Trim() 'versions' }
    } catch { }
    return Join-Path $data 'versions'
}

# Names of the DLLs a PE file imports, read from its import directory (the game exe is 300 MB).
function Get-PeImport([Parameter(Mandatory)][string]$Path) {
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $r = New-Object IO.BinaryReader($fs)
        $fs.Position = 0x3C; $pe = $r.ReadInt32()
        $fs.Position = $pe + 6; $sectionCount = $r.ReadUInt16()
        $fs.Position = $pe + 20; $optionalSize = $r.ReadUInt16()
        $optional = $pe + 24
        $fs.Position = $optional
        $dataDirs = $optional + $(if ($r.ReadUInt16() -eq 0x20B) { 112 } else { 96 })
        $fs.Position = $dataDirs + 8; $importRva = $r.ReadUInt32()
        $sections = foreach ($i in 0..($sectionCount - 1)) {
            $fs.Position = $optional + $optionalSize + 40 * $i + 8
            $virtualSize = $r.ReadUInt32(); $va = $r.ReadUInt32(); $rawSize = $r.ReadUInt32(); $raw = $r.ReadUInt32()
            @{ Va = [int64]$va; End = [int64]$va + [Math]::Max($virtualSize, $rawSize); Raw = [int64]$raw }
        }
        $offset = { param([int64]$Rva) foreach ($s in $sections) { if ($Rva -ge $s.Va -and $Rva -lt $s.End) { return $s.Raw + $Rva - $s.Va } }; return -1 }
        $names = @()
        $d = & $offset $importRva
        if ($importRva -eq 0 -or $d -lt 0) { return $names }
        for (; ; $d += 20) {
            $fs.Position = $d + 12
            $nameRva = $r.ReadUInt32()
            if ($nameRva -eq 0) { break }
            $fs.Position = & $offset $nameRva
            $bytes = New-Object System.Collections.Generic.List[byte]
            while (($b = $r.ReadByte()) -ne 0) { $bytes.Add($b) }
            $names += [Text.Encoding]::ASCII.GetString($bytes.ToArray())
        }
        return $names
    } finally {
        $fs.Dispose()
    }
}

# The base instance (not a clone) of Minecraft $GameVersion, preferring one with LeviLamina, or $null.
function Find-BeDDBaseInstance([string]$VersionsDir = (Get-BeDDVersionsDir)) {
    if ($env:BEDD_INSTANCE) { return $env:BEDD_INSTANCE }
    $found = foreach ($d in Get-ChildItem $VersionsDir -Directory -ErrorAction SilentlyContinue) {
        if ($d.Name -match '-hi\d+$' -or -not (Test-Path (Join-Path $d.FullName 'Minecraft.Windows.exe'))) { continue }
        $meta = $null
        try { $meta = Get-Content (Join-Path $d.FullName 'version.json') -Raw -ErrorAction Stop | ConvertFrom-Json } catch { }
        $version = if ($meta -and $meta.gameVersion) { $meta.gameVersion } else { $d.Name }
        if ($version -ne $script:GameVersion -and $version -notlike "$script:GameVersion.*") { continue }
        if ($meta -and $meta.type -and $meta.type -ne 'release') { continue }
        [pscustomobject]@{
            Name = $d.Name
            Rank = [int](Test-Path (Join-Path $d.FullName 'mods\LeviLamina\LeviLamina.dll')) * 2 + [int]($d.Name -eq $version)
        }
    }
    $best = $found | Sort-Object @{ Expression = 'Rank'; Descending = $true }, Name | Select-Object -First 1
    if ($best) { return $best.Name }
    return $null
}

# An instance and what it still lacks, without changing anything. -Name picks it (a clone is
# <base>-hi<N>); otherwise the base instance, or its clone for -Slot 2 and up.
function Get-BeDDInstance([string]$Name, [int]$Slot = 1, [string]$VersionsDir) {
    if (-not $VersionsDir) { $VersionsDir = Get-BeDDVersionsDir }
    if (-not $Name) {
        $base = Find-BeDDBaseInstance $VersionsDir
        if (-not $base) { $base = "$script:GameVersion.01" } # what LeviLauncher names a new one
        $Name = if ($Slot -gt 1) { "$base-hi$Slot" } else { $base }
    }
    $base = $Name; $Slot = 1
    if ($Name -match '^(.+)-hi(\d+)$') { $base = $Matches[1]; $Slot = [int]$Matches[2] }
    $dir = Join-Path $VersionsDir $Name
    $exe = Join-Path $dir 'Minecraft.Windows.exe'
    $problems = @()
    if (-not (Test-Path $exe)) {
        $problems += 'missing'
    } else {
        $imports = @()
        try { $imports = Get-PeImport $exe } catch { Write-Warning "[BeDD] could not read the imports of ${exe}: $($_.Exception.Message)" }
        if (-not (Test-Path (Join-Path $dir 'mods\LeviLamina\LeviLamina.dll')) -or $imports -notcontains 'PreLoader.dll') { $problems += 'leviLamina' }
        if (-not (Test-Path (Join-Path $dir 'LeviLauncher.dll')) -or $imports -notcontains 'LeviLauncher.dll') { $problems += 'loader' }
    }
    return [pscustomobject]@{
        Name        = $Name
        Dir         = $dir
        ModsDir     = Join-Path $dir 'mods'
        Base        = $base
        Slot        = $Slot
        VersionsDir = $VersionsDir
        PipeName    = "BeDD-$Name"
        Problems    = $problems
    }
}

# Downloads a GitHub release asset (the latest release unless -Tag) whose name matches -Asset.
function Get-GitHubAsset([string]$Repo, [string]$Asset, [string]$OutFile, [string]$Tag) {
    $api = if ($Tag) { "https://api.github.com/repos/$Repo/releases/tags/$Tag" } else { "https://api.github.com/repos/$Repo/releases/latest" }
    $release = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'BeDD' }
    $a = $release.assets | Where-Object { $_.name -like $Asset } | Select-Object -First 1
    if (-not $a) { throw "No asset like '$Asset' in $Repo $($release.tag_name)" }
    New-Item -ItemType Directory -Force (Split-Path $OutFile) | Out-Null
    Write-Setup "downloading $($a.name) ($Repo $($release.tag_name))"
    $tmp = "$OutFile.download"
    $saved = $ProgressPreference
    try {
        $ProgressPreference = 'SilentlyContinue' # much faster in Windows PowerShell
        Invoke-WebRequest -Uri $a.browser_download_url -OutFile $tmp -UseBasicParsing
    } finally {
        $ProgressPreference = $saved
    }
    Move-Item -Force $tmp $OutFile
    return $OutFile
}

# LeviLauncher.exe: the one installed (or running), else a download into %LOCALAPPDATA%\BeDD\tools
# with -Download. It keeps its data in %APPDATA%\levilauncher.exe whichever copy runs.
function Find-LeviLauncher([switch]$Download) {
    if ($env:BEDD_LEVILAUNCHER) { return $env:BEDD_LEVILAUNCHER }
    $running = Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue | Where-Object { $_.Path } | Select-Object -First 1
    if ($running) { return $running.Path }
    $candidates = @()
    foreach ($key in 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
            'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*') {
        Get-ItemProperty $key -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like 'LeviLauncher*' -and $_.InstallLocation } |
            ForEach-Object { $candidates += Join-Path $_.InstallLocation 'LeviLauncher.exe' }
    }
    $candidates += @(
        (Join-Path $env:ProgramFiles 'LeviMC\LeviLauncher\LeviLauncher.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\LeviLauncher\LeviLauncher.exe'),
        (Join-Path ([Environment]::GetFolderPath('Desktop')) 'LeviLauncher.exe'),
        (Join-Path $env:USERPROFILE 'Downloads\LeviLauncher.exe'),
        (Join-Path $script:ToolsDir 'LeviLauncher\LeviLauncher.exe'))
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    if (-not $Download) { return $null }
    return Get-GitHubAsset 'LiteLDev/LeviLauncher' 'LeviLauncher.exe' (Join-Path $script:ToolsDir 'LeviLauncher\LeviLauncher.exe')
}

# lip.exe (on PATH, or downloaded) and the .NET runtime it runs on. Sets DOTNET_ROOT for this
# process when the runtime is the per-user one downloaded here.
function Get-BeDDLip {
    $lip = $env:BEDD_LIP
    if (-not $lip) {
        $c = Get-Command lip -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($c) { $lip = $c.Source }
    }
    if (-not $lip) {
        $lip = Join-Path $script:ToolsDir 'lip\lip.exe'
        if (-not (Test-Path $lip)) {
            $zip = Get-GitHubAsset 'LiteLDev/lip' 'lip-*-win-x64.zip' (Join-Path $script:ToolsDir 'lip.zip')
            Expand-Archive -Force $zip (Split-Path $lip)
            Remove-Item $zip
        }
    }
    # lip is a .NET 10 program; LeviLauncher installs the same runtime the same way.
    $hasRuntime = { param($root) $root -and (Get-ChildItem (Join-Path $root 'shared\Microsoft.NETCore.App\10.*') -Directory -ErrorAction SilentlyContinue) }
    $userDotnet = Join-Path $env:LOCALAPPDATA 'Microsoft\dotnet'
    if (-not ((& $hasRuntime $env:DOTNET_ROOT) -or (& $hasRuntime (Join-Path $env:ProgramFiles 'dotnet')))) {
        if (-not (& $hasRuntime $userDotnet)) {
            Write-Setup 'installing the .NET 10 runtime for lip (per user)'
            $script = Join-Path $script:ToolsDir 'dotnet-install.ps1'
            New-Item -ItemType Directory -Force $script:ToolsDir | Out-Null
            Invoke-WebRequest -Uri 'https://dot.net/v1/dotnet-install.ps1' -OutFile $script -UseBasicParsing
            & $script -Runtime dotnet -Channel 10.0 -InstallDir $userDotnet -NoPath | Out-Host
            if (-not (& $hasRuntime $userDotnet)) { throw "The .NET 10 runtime for lip did not install into $userDotnet" }
        }
        $env:DOTNET_ROOT = $userDotnet
    }
    return $lip
}

function Install-BeDDLeviLamina($Instance) {
    $lip = Get-BeDDLip
    $package = "github.com/LiteLDev/LeviLamina#client@$script:LeviLaminaVersion"
    Write-Setup "installing LeviLamina $script:LeviLaminaVersion into $($Instance.Name) with lip"
    Push-Location $Instance.Dir
    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue' # lip's stderr would stop Windows PowerShell
        & $lip install $package 2>&1 | Out-Host
        if ($LASTEXITCODE) { throw "lip install $package failed in $($Instance.Dir) (exit code $LASTEXITCODE)" }
    } finally {
        $ErrorActionPreference = $saved
        Pop-Location
    }
}

# Games of one instance (matched by its folder: every instance runs a Minecraft.Windows.exe).
function Get-InstanceGame($Instance) {
    $prefix = $Instance.Dir.TrimEnd('\') + '\'
    Get-Process -Name 'Minecraft.Windows' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) }
}

# LeviLauncher adds its loader (LeviLauncher.dll: the instance's own game data, mods) to the game exe
# when it starts an instance. Starts the instance through it once and closes the game again.
function Add-LeviLauncherLoader($Instance, [int]$TimeoutMinutes) {
    $launcher = Find-LeviLauncher -Download
    $wasRunning = [bool](Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue)
    Write-Setup "starting $($Instance.Name) once through LeviLauncher, which adds its loader to the game (LeviLauncher may ask to install the VC++ runtime or GameInput: accept)"
    Start-Process -FilePath $launcher -ArgumentList "--launch=`"$($Instance.Name)`"" | Out-Null
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    try {
        while ((Get-BeDDInstance $Instance.Name -VersionsDir $Instance.VersionsDir).Problems -contains 'loader') {
            if ((Get-Date) -gt $deadline) { throw "LeviLauncher did not prepare $($Instance.Name) within $TimeoutMinutes min. Start it there once by hand." }
            Start-Sleep -Seconds 2
        }
        # The game it starts now (closed before it gets far).
        $gameDeadline = (Get-Date).AddSeconds(60)
        while (-not (Get-InstanceGame $Instance) -and (Get-Date) -lt $gameDeadline) { Start-Sleep -Milliseconds 500 }
    } finally {
        Get-InstanceGame $Instance | Stop-Process -Force -ErrorAction SilentlyContinue
        if (-not $wasRunning) { Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
    }
    Write-Setup "$($Instance.Name) has LeviLauncher's loader"
}

# Opens LeviLauncher and waits until the person at the PC has installed the game there.
function Wait-BeDDGameInstall([string]$VersionsDir, [int]$TimeoutMinutes) {
    $how = "Install Minecraft $script:GameVersion (Release) in LeviLauncher: sign in with a Microsoft account that owns Minecraft, " +
        "then download $script:GameVersion.x with LeviLamina. BeDD does the rest."
    if (-not [Environment]::UserInteractive -or $env:CI) { throw "[BeDD] No Minecraft $script:GameVersion instance in $VersionsDir. $how" }
    $launcher = Find-LeviLauncher -Download
    if (-not (Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue)) { Start-Process -FilePath $launcher | Out-Null }
    Write-Setup "No Minecraft $script:GameVersion instance in $VersionsDir yet: LeviLauncher is open."
    Write-Setup $how
    Write-Setup "Waiting up to $TimeoutMinutes min for it..."
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    while (-not (Find-BeDDBaseInstance $VersionsDir)) {
        if ((Get-Date) -gt $deadline) { throw "[BeDD] No Minecraft $script:GameVersion instance after $TimeoutMinutes min. $how" }
        Start-Sleep -Seconds 5
    }
    # LeviLauncher writes the game files before it is done (version.json, LeviLamina): wait for quiet.
    Start-Sleep -Seconds 30
}

# Makes the instance ready for headless games and returns it (Get-BeDDInstance). Fast when nothing
# is missing; one setup at a time on the PC (several runs may start at once).
function Initialize-BeDDInstance([string]$Name, [int]$Slot = 1, [string]$VersionsDir, [int]$TimeoutMinutes = 30) {
    if (-not $VersionsDir) { $VersionsDir = Get-BeDDVersionsDir }
    $instance = Get-BeDDInstance $Name -Slot $Slot -VersionsDir $VersionsDir
    if (-not $instance.Problems) { return $instance }

    $mutex = New-Object System.Threading.Mutex($false, 'Global\BeDD.Setup')
    $owned = $false
    try {
        try { $owned = $mutex.WaitOne([TimeSpan]::FromMinutes($TimeoutMinutes + 5)) } catch [System.Threading.AbandonedMutexException] { $owned = $true }
        if (-not $owned) { throw '[BeDD] another BeDD setup did not finish' }

        $base = Get-BeDDInstance $instance.Base -VersionsDir $VersionsDir
        if ($base.Problems -contains 'missing') {
            if ($Name -and $instance.Base -notlike "$script:GameVersion*") { throw "[BeDD] No game instance at $($base.Dir)" }
            Wait-BeDDGameInstall $VersionsDir $TimeoutMinutes
            if (-not $Name) { $instance = Get-BeDDInstance -Slot $instance.Slot -VersionsDir $VersionsDir }
            $base = Get-BeDDInstance $instance.Base -VersionsDir $VersionsDir
            if ($base.Problems -contains 'missing') {
                throw "[BeDD] Minecraft was installed as '$(Find-BeDDBaseInstance $VersionsDir)', not '$($instance.Base)': use that name, or leave the name out"
            }
        }
        if ($base.Problems -contains 'leviLamina') {
            Install-BeDDLeviLamina $base
            $base = Get-BeDDInstance $base.Name -VersionsDir $VersionsDir
            if ($base.Problems -contains 'leviLamina') { throw "[BeDD] LeviLamina is still missing in $($base.Dir) after lip install" }
        }
        if ($base.Problems -contains 'loader') { Add-LeviLauncherLoader $base $TimeoutMinutes }
        if ($instance.Slot -gt 1) { New-BeDDClone -Source $base.Name -Slot $instance.Slot -VersionsDir $VersionsDir }

        $instance = Get-BeDDInstance $instance.Name -VersionsDir $VersionsDir
        if ($instance.Problems) { throw "[BeDD] $($instance.Name) is not ready ($($instance.Problems -join ', '))" }
        return $instance
    } finally {
        if ($owned) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

# Clones the instance <Source> into <Source>-hi<Slot> cheaply (see clone-instance.ps1), or brings an
# existing clone's game files up to date with the source (a source updated since got new files,
# which a hard link does not follow). -Force rebuilds the clone from scratch.
function New-BeDDClone([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][int]$Slot, [string]$VersionsDir, [switch]$Force) {
    if (-not $VersionsDir) { $VersionsDir = Get-BeDDVersionsDir }
    $src = Join-Path $VersionsDir $Source
    $name = "$Source-hi$Slot"
    $dst = Join-Path $VersionsDir $name
    if (-not (Test-Path (Join-Path $src 'Minecraft.Windows.exe'))) { throw "No instance at $src" }

    $junctions = @('data', 'Installers')
    $copies    = @('Minecraft Bedrock')
    $skip      = @('logs', 'mods', 'mods-disabled')

    if ((Test-Path $dst) -and $Force) {
        # Junctions are removed without touching their targets; hard links only drop a name.
        Get-ChildItem $dst -Force | Where-Object { $_.LinkType -eq 'Junction' } | ForEach-Object { $_.Delete() }
        Remove-Item -Recurse -Force $dst
    }
    if (Test-Path $dst) {
        # A hard link has the size and time of its source; a file that differs was replaced there.
        $stale = 0
        foreach ($item in Get-ChildItem $src -File -Force) {
            if ($item.Name -eq 'version.json') { continue }
            $target = Join-Path $dst $item.Name
            $t = Get-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
            if ($t -and $t.Length -eq $item.Length -and $t.LastWriteTimeUtc -eq $item.LastWriteTimeUtc) { continue }
            try {
                if ($t) { Remove-Item -LiteralPath $target -Force -ErrorAction Stop }
                New-Item -ItemType HardLink -Path $target -Target $item.FullName | Out-Null
                $stale++
            } catch {
                Write-Warning "[BeDD] could not refresh $target from ${src}: $($_.Exception.Message)"
            }
        }
        foreach ($m in 'LeviLamina', 'CrashLogger') {
            $p = Join-Path $src "mods\$m"
            if ((Test-Path $p) -and -not (Test-Path (Join-Path $dst "mods\$m"))) { Copy-Item -Recurse $p (Join-Path $dst "mods\$m"); $stale++ }
        }
        if ($stale) { Write-Setup "clone $name refreshed from $Source ($stale item(s))" }
        return
    }

    New-Item -ItemType Directory $dst | Out-Null
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
    Write-Setup "clone $name ready at $dst"
}

Export-ModuleMember -Function Get-LeviLauncherData, Get-BeDDVersionsDir, Find-BeDDBaseInstance, Get-BeDDInstance,
    Initialize-BeDDInstance, New-BeDDClone, Find-LeviLauncher, Get-BeDDLip
