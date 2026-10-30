<#
.SYNOPSIS
  Records the showcase video: three headless games driven from a terminal, as the manager window
  shows them.

.DESCRIPTION
  Takes the machine-wide e2e lock (bei-e2e.lock, shared with the other LiteLDev harnesses) and the
  locks of the clone slots it uses, prepares the clone instances versions\<base>-hi<N> (other mods
  moved aside, BeDD deployed with its own pipe, the Showcase world restored), starts
  the games without activating them, enters the world in each and stages the scene through chat
  commands.

  The games run as every BeDD game does: headless, shown only in a manager window. The recording
  has a manager window of its own (BEDD_MANAGER_PIPE), so other runs' games stay out of the take. A
  take sizes that manager's client area to the left part of a 1920x1080 picture (physical pixels),
  opens a console window off the screen and without the focus running demo.ps1 (the script viewers
  read) for the right part, and records both windows with ffmpeg's gfxcapture
  (Windows.Graphics.Capture: the windows' own pictures, whatever covers them). Nothing is moved in
  front of you and nothing takes the focus; the manager gets its size back afterwards.

  -KeepOpen keeps the games running after the first take: create tools\showcase\out\take for
  another take (demo.ps1 is re-read), tools\showcase\out\stop to end. Cleanup always closes the
  games, removes the mod, restores the other mods and options and releases the locks.

  Output: tools\showcase\out\take-<n>-manager.mkv and take-<n>-terminal.mkv (near-lossless) and
  setup screenshots; encode.ps1 puts a take together into the files in docs\public\showcase\.

.EXAMPLE
  pwsh -NoProfile -File tools/showcase/record.ps1 -KeepOpen
#>
param(
    [int[]]$Slots = @(2, 3, 4),
    [string]$BaseVersion,           # default: the instance BeDD finds (BeDDSetup.psm1)
    [int]$TakeSeconds = 24,
    [switch]$KeepOpen,
    [int]$MaxMinutes = 60
)
$ErrorActionPreference = 'Stop'

$Root      = (Resolve-Path "$PSScriptRoot/../..").Path
$Bin       = Join-Path $Root 'bin\BeDD'
$Out       = Join-Path $PSScriptRoot 'out'
$WorldZip  = Join-Path $PSScriptRoot 'fixtures\Showcase.zip'
$WorldName = 'LHI-Showcase'
$Aside     = 'mods.lhi-showcase-aside'
# Game client size (16:9, as the manager window shows it), the picture (physical pixels) and the
# manager's share of its width (the terminal gets the rest).
$ClientW = 848; $ClientH = 477
$RegionW = 1920; $RegionH = 1080
$ManagerW = 1248

if (-not (Test-Path (Join-Path $Bin 'BeDD.dll'))) { throw "Build first (xmake): $Bin" }
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item (Join-Path $Out 'take'), (Join-Path $Out 'stop'), (Join-Path $Out 'go') -ErrorAction SilentlyContinue

# Set-WorldSinglePlayer, Install-/Uninstall-BeDDMod, Enter-/Exit-SignalingLock and the
# BeDD.psm1 client (per-monitor DPI aware: all coordinates below are physical pixels).
. (Join-Path $Bin 'harness-shim.ps1')
. (Join-Path $PSScriptRoot 'ffmpeg.ps1')
$FFmpeg      = Find-FFmpeg
$Data        = Get-LeviLauncherData
$VersionsDir = Get-BeDDVersionsDir
if (-not $BaseVersion) { $BaseVersion = (Initialize-BeDDInstance).Name }

if (-not ('LhiShow.Win' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace LhiShow {
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool SetWindowText(IntPtr h, string s);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
    public static IntPtr FindByTitle(string title, bool visibleOnly) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            if (visibleOnly && !IsWindowVisible(h)) return true;
            var s = new System.Text.StringBuilder(256); GetWindowText(h, s, 256);
            if (s.ToString() == title) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
}
'@
}
Add-Type -AssemblyName System.Windows.Forms
$SWP_NOMOVE = 0x2; $SWP_NOZORDER = 0x4; $SWP_NOACTIVATE = 0x10

function Write-Show([string]$m) { Write-Host "[showcase] $m" }

# --- instances ----------------------------------------------------------------------------------

$Games = @(for ($i = 0; $i -lt $Slots.Count; $i++) {
    $ver = "$BaseVersion-hi$($Slots[$i])"
    [pscustomobject]@{
        N = $i + 1; Slot = $Slots[$i]; Version = $ver
        Dir = Join-Path $VersionsDir $ver
        Pipe = "BeDD-$ver"
        Lock = Join-Path $Data "hi$($Slots[$i])-e2e.lock"
        Options = $null
    }
})

function Get-InstanceProcesses($g) {
    $dir = $g.Dir.TrimEnd('\') + '\'
    Get-Process -Name 'Minecraft.Windows' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) }
}

function Get-WorldsDir($g) {
    $dirs = @(Get-ChildItem (Join-Path $g.Dir 'Minecraft Bedrock\Users') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'games\com.mojang\minecraftWorlds' } | Where-Object { Test-Path $_ })
    $user = $dirs | Where-Object { $_ -notmatch '\\Shared\\' } | Select-Object -First 1
    if ($user) { return $user }
    return $dirs | Select-Object -First 1
}

function Get-OptionsFile($g) {
    $all = @(Get-ChildItem (Join-Path $g.Dir 'Minecraft Bedrock') -Recurse -Filter options.txt -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\minecraftpe\\options\.txt$' })
    $user = $all | Where-Object { $_.FullName -notmatch '\\Users\\Shared\\' } | Select-Object -First 1
    if ($user) { return $user.FullName }
    return $all | Select-Object -First 1 -ExpandProperty FullName
}

# Writes a value into a level.dat tag (Bedrock: 8-byte header, little-endian NBT).
function Set-LevelTag([string]$WorldDir, [byte]$Type, [string]$Name, [byte[]]$Value) {
    foreach ($f in 'level.dat', 'level.dat_old') {
        $path = Join-Path $WorldDir $f
        if (-not (Test-Path $path)) { continue }
        $b = [IO.File]::ReadAllBytes($path)
        $nb = [Text.Encoding]::UTF8.GetBytes($Name)
        $pat = [byte[]](@($Type, [byte]($nb.Length -band 0xFF), [byte]($nb.Length -shr 8)) + $nb)
        for ($i = 8; $i -le $b.Length - $pat.Length; $i++) {
            $hit = $true
            for ($j = 0; $j -lt $pat.Length; $j++) { if ($b[$i + $j] -ne $pat[$j]) { $hit = $false; break } }
            if (-not $hit) { continue }
            [Array]::Copy($Value, 0, $b, $i + $pat.Length, $Value.Length)
            break
        }
        [IO.File]::WriteAllBytes($path, $b)
    }
}

function Restore-Mods($g) {
    $aside = Join-Path $g.Dir $Aside
    if (-not (Test-Path $aside)) { return }
    foreach ($d in Get-ChildItem $aside -Directory) {
        $dest = Join-Path $g.Dir "mods\$($d.Name)"
        if (Test-Path $dest) { Move-Item $d.FullName "$dest.stale-$(Get-Date -Format yyyyMMddHHmmss)" } else { Move-Item $d.FullName $dest }
    }
    Remove-Item $aside -Recurse -Force -ErrorAction SilentlyContinue
}

function Initialize-Instance($g) {
    & (Join-Path $Bin 'clone-instance.ps1') -Slot $g.Slot -Source $BaseVersion
    if (Get-InstanceProcesses $g) { throw "A game of $($g.Version) is already running" }
    Restore-Mods $g   # leftovers of a killed run
    $aside = Join-Path $g.Dir $Aside
    New-Item -ItemType Directory -Force $aside | Out-Null
    foreach ($d in Get-ChildItem (Join-Path $g.Dir 'mods') -Directory) {
        if ($d.Name -in 'LeviLamina', 'BeDD') { continue }
        Move-Item $d.FullName (Join-Path $aside $d.Name)
    }
    $script:WindowW = 1600; $script:WindowH = 900
    Install-BeDDMod -ModsDir (Join-Path $g.Dir 'mods') -PipeName $g.Pipe

    $worlds = Get-WorldsDir $g
    if (-not $worlds) { throw "No worlds folder in $($g.Dir) (start the instance once)" }
    Get-ChildItem $worlds -Directory | Where-Object {
        (Get-Content (Join-Path $_.FullName 'levelname.txt') -ErrorAction SilentlyContinue) -eq $WorldName
    } | Remove-Item -Recurse -Force
    $w = Join-Path $worlds $WorldName
    Expand-Archive -Path $WorldZip -DestinationPath $w -Force
    Set-Content -NoNewline -Encoding utf8NoBOM (Join-Path $w 'levelname.txt') $WorldName
    Set-LevelTag $w 4 'LastPlayed' ([BitConverter]::GetBytes([long][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()))
    Set-LevelTag $w 1 'domobspawning' @([byte]0)
    Set-WorldSinglePlayer $w

    $g.Options = Get-OptionsFile $g
    if ($g.Options) {
        Copy-Item $g.Options "$($g.Options).lhi-showcase" -Force
        (Get-Content $g.Options) -replace '^gfx_hidehud:.*', 'gfx_hidehud:0' -replace '^gfx_guiscale_offset:.*', 'gfx_guiscale_offset:1' | Set-Content -Encoding utf8NoBOM $g.Options
    }
    Write-Show "prepared $($g.Version)"
}

function Reset-Instance($g) {
    foreach ($p in Get-InstanceProcesses $g) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(10000)) { $p.Kill() } }
    try { Uninstall-BeDDMod -ModsDir (Join-Path $g.Dir 'mods') } catch { Write-Show "removing the mod failed: $_" }
    try { Restore-Mods $g } catch { Write-Show "restoring mods failed: $_" }
    if ($g.Options -and (Test-Path "$($g.Options).lhi-showcase")) { Move-Item "$($g.Options).lhi-showcase" $g.Options -Force }
}

# --- driving ------------------------------------------------------------------------------------

function Use-Game($g) { Set-BeDDPipe $g.Pipe }
function Invoke-Chat($g, [string]$Text) {
    Use-Game $g
    Send-BeDDKey T; Start-Sleep -Milliseconds 700
    Send-BeDDText $Text
    Send-BeDDKey ENTER; Start-Sleep -Milliseconds 500
}
function Save-Shot($g, [string]$Name) { Use-Game $g; Save-BeDDScreenshot (Join-Path $Out "g$($g.N)-$Name.png") | Out-Null }

# Title screen: the green Play button (fraction of the client area).
function Test-PlayButton($g) {
    Use-Game $g
    $bmp = Get-BeDDBitmap
    try { $c = $bmp.GetPixel([int]($bmp.Width * 0.47), [int]($bmp.Height * 0.474)) } finally { $bmp.Dispose() }
    return ($c.G -gt $c.R + 30 -and $c.G -gt $c.B + 30)
}

function Get-LogLines($g) { @(Get-Content (Join-Path $g.Dir 'logs\latest.log') -ErrorAction SilentlyContinue) }

function Enter-World($g) {
    Use-Game $g
    Send-BeDDMouse click -RX 0.498 -RY 0.474; Start-Sleep -Seconds 4     # Play
    Save-Shot $g 'worldlist'
    Send-BeDDMouse click -RX $TileX[0] -RY 0.43                          # first world (LastPlayed = now)
}
# World list tiles (fractions of the 1600x900 client). The first tile can be a featured server
# shown as a "LAN world" (it sorts before local worlds): then the test world is the next tile.
$TileX = @(0.173, 0.383, 0.593, 0.803)

# Waits for "Player Spawned" in the instance's log after line $After; clicks the world again when
# the first click did not open it (the world list sometimes takes longer to fill).
function Wait-InWorld($g, [int]$After) {
    for ($attempt = 1; $attempt -le 4; $attempt++) {
        $deadline = (Get-Date).AddSeconds(40)
        while ((Get-Date) -lt $deadline) {
            if ((Get-LogLines $g | Select-Object -Skip $After) -match 'Player Spawned') { Write-Show "game $($g.N): in the world"; return }
            Start-Sleep -Seconds 1
        }
        Save-Shot $g "join-$attempt"
        if ($attempt -ge $TileX.Count) { break }
        Write-Show "game $($g.N): not in the test world yet, cancelling and trying tile $($attempt + 1)"
        Use-Game $g
        Send-BeDDMouse click -RX 0.5 -RY 0.574; Start-Sleep -Seconds 5       # Cancel "Connecting to multiplayer game"
        Save-Shot $g "after-cancel-$attempt"
        if (Test-PlayButton $g) { Send-BeDDMouse click -RX 0.498 -RY 0.474; Start-Sleep -Seconds 4 }
        Send-BeDDMouse click -RX $TileX[$attempt] -RY 0.43
    }
    throw "game $($g.N) did not enter the world"
}

# Per game: facing (yaw pitch) and the items game 3 shows in its inventory.
$Spawn = '~ ~ ~'
$Facing = @{ 1 = '0 2'; 2 = '90 12'; 3 = '180 5' }
$sceneFile = Join-Path $PSScriptRoot 'scene.ps1'   # optional overrides of $Spawn / $Facing, re-read every take
function Set-Scene($g) {
    Invoke-Chat $g '/kill @e[type=!player]'
    Invoke-Chat $g '/kill @e[type=item]'   # what the animals dropped
    Invoke-Chat $g "/tp @s $Spawn $($Facing[$g.N])"
    if ($g.N -eq 3) {
        Invoke-Chat $g '/clear @s'
        foreach ($i in 'diamond_sword 1', 'golden_apple 4', 'cooked_beef 16', 'oak_log 32', 'torch 24', 'cake 1') { Invoke-Chat $g "/give @s $i" }
    }
    Use-Game $g; Clear-BeDDInput
    Disconnect-BeDD   # the pipe serves one client at a time: leave it to demo.ps1
}

# The recording's own manager window (BeDDManager.ps1 -PipeName $ManagerPipe, in pwsh).
$ManagerPipe = "BeDD-Manager-showcase-$PID"
function Get-ManagerWindow {
    $p = Get-CimInstance Win32_Process -Filter "Name = 'pwsh.exe'" | Where-Object {
        $_.CommandLine -match 'BeDDManager\.ps1' -and $_.CommandLine -like "*$ManagerPipe*" -and $_.CommandLine -notmatch ' -(Ensure|List|Adopt|Game|Shutdown)' }
    foreach ($m in $p) {
        $h = (Get-Process -Id $m.ProcessId -ErrorAction SilentlyContinue).MainWindowHandle
        if ($h -and $h -ne [IntPtr]::Zero) { return $h }
    }
    throw 'the BeDD manager window was not found'
}

function Show-Take([int]$Take) {
    if (Test-Path $sceneFile) { . $sceneFile }
    Write-Show "take ${Take}: staging the scene"
    foreach ($g in $Games) { Set-Scene $g }
    Start-Sleep -Seconds 12   # chat fades

    $mh = Get-ManagerWindow
    $saved = New-Object LhiShow.Win+RECT; [LhiShow.Win]::GetWindowRect($mh, [ref]$saved) | Out-Null
    $proc = $null
    try {
        # The manager's client area at its share of the picture; it stays where it is.
        $cr = New-Object LhiShow.Win+RECT; [LhiShow.Win]::GetClientRect($mh, [ref]$cr) | Out-Null
        [LhiShow.Win]::SetWindowPos($mh, [IntPtr]::Zero, 0, 0, $ManagerW + ($saved.Right - $saved.Left) - $cr.Right,
            $RegionH + ($saved.Bottom - $saved.Top) - $cr.Bottom, $SWP_NOMOVE -bor $SWP_NOZORDER -bor $SWP_NOACTIVATE) | Out-Null

        # The terminal the script runs in: a console window off the screen, shown without the focus.
        Remove-Item (Join-Path $Out 'go') -ErrorAction SilentlyContinue
        $title = 'e2e - my-client-mod'
        $pipes = ($Games | ForEach-Object Pipe) -join ','
        $proc = Start-Process "$env:WINDIR\System32\conhost.exe" -PassThru -WindowStyle Hidden -ArgumentList @(
            "`"$((Get-Command pwsh).Source)`"", '-NoLogo', '-NoProfile', '-File', "`"$(Join-Path $PSScriptRoot 'demo.ps1')`"",
            '-Pipes', $pipes, '-GoFile', "`"$(Join-Path $Out 'go')`"", '-LingerSeconds', $TakeSeconds)
        $th = [IntPtr]::Zero
        for ($i = 0; $i -lt 60 -and $th -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $th = [LhiShow.Win]::FindByTitle($title, $false) }
        if ($th -eq [IntPtr]::Zero) { throw 'terminal window not found' }
        $left = [System.Windows.Forms.SystemInformation]::VirtualScreen.Left - ($RegionW - $ManagerW) - 200   # left of every monitor
        [LhiShow.Win]::SetWindowPos($th, [IntPtr]::Zero, $left, 0, $RegionW - $ManagerW, $RegionH, $SWP_NOZORDER -bor $SWP_NOACTIVATE) | Out-Null
        [LhiShow.Win]::ShowWindow($th, 4) | Out-Null   # SW_SHOWNOACTIVATE
        Start-Sleep -Seconds 1

        # Both windows straight from the compositor (Windows.Graphics.Capture), whatever covers them.
        $cap = { param($h) "gfxcapture=hwnd=$($h.ToInt64()):capture_cursor=0:max_framerate=30,hwdownload,format=bgra,fps=30" }
        $enc = @('-t', $TakeSeconds, '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '10', '-pix_fmt', 'yuv444p')
        $files = @((Join-Path $Out "take-$Take-manager.mkv"), (Join-Path $Out "take-$Take-terminal.mkv"))
        $ff = Start-Process $FFmpeg -PassThru -WindowStyle Hidden -ArgumentList (@('-hide_banner', '-loglevel', 'error', '-y',
            '-filter_complex', "$(& $cap $mh)[m];$(& $cap $th)[t]",
            '-map', '[m]') + $enc + "`"$($files[0])`"" + @('-map', '[t]') + $enc + "`"$($files[1])`"")
        Start-Sleep -Seconds 2
        New-Item -ItemType File (Join-Path $Out 'go') | Out-Null
        Write-Show "take ${Take}: recording $TakeSeconds s -> $($files -join ', ')"
        # A window capture delivers frames only when the window changes: once the script is done the
        # terminal is still, so closing it is what ends its recording.
        Start-Sleep -Seconds ($TakeSeconds + 1)
        if (-not $proc.HasExited) { $proc.Kill($true) }
        if (-not $ff.WaitForExit(30000)) { $ff.Kill(); throw "ffmpeg did not finish the take" }
    } finally {
        [LhiShow.Win]::SetWindowPos($mh, [IntPtr]::Zero, $saved.Left, $saved.Top, $saved.Right - $saved.Left, $saved.Bottom - $saved.Top,
            $SWP_NOZORDER -bor $SWP_NOACTIVATE) | Out-Null
        if ($proc -and -not $proc.HasExited) { $proc.Kill($true) }
        foreach ($g in $Games) { Use-Game $g; try { Clear-BeDDInput } catch {} }
    }
}

# --- run ----------------------------------------------------------------------------------------

$locks = @()
function Enter-Lock([string]$Path) {
    $waited = $false
    while ($true) {
        try {
            $s = [IO.File]::Open($Path, 'OpenOrCreate', 'ReadWrite', 'None')
            $bytes = [Text.Encoding]::UTF8.GetBytes("pid=$PID repo=$Root at=$(Get-Date -Format o)")
            $s.SetLength(0); $s.Write($bytes, 0, $bytes.Length); $s.Flush()
            return $s
        } catch [System.IO.IOException] {
            if (-not $waited) { Write-Show "waiting for $(Split-Path $Path -Leaf)"; $waited = $true }
            Start-Sleep -Seconds 2
        }
    }
}

try {
    $locks += Enter-Lock (Join-Path $Data 'bei-e2e.lock')
    foreach ($g in $Games) { $locks += Enter-Lock $g.Lock }
    Write-Show 'locks acquired'
    foreach ($g in $Games) { Initialize-Instance $g }
    Enter-SignalingLock
    # Inherited by the scripts below and the games they start: they all use the recording's manager.
    $env:BEDD_MANAGER_PIPE = $ManagerPipe
    if (-not (& (Join-Path $Bin 'BeDDManager.ps1') -Ensure)) { throw 'the manager window did not start' }

    foreach ($g in $Games) {
        & (Join-Path $Bin 'Start-BeDDGame.ps1') -Instance $g.Version -Label "game $($g.N)" | Out-Null
        Write-Show "started $($g.Version)"
        Start-Sleep -Seconds 3
    }
    foreach ($g in $Games) {
        Connect-BeDD -PipeName $g.Pipe -TimeoutSeconds 240 | Out-Null
        $deadline = (Get-Date).AddSeconds(180)
        while (-not (Test-PlayButton $g)) {
            if ((Get-Date) -gt $deadline) { Save-Shot $g 'no-title'; throw "game $($g.N): title screen not found" }
            Start-Sleep -Seconds 2
        }
        Start-Sleep -Seconds 3
        $g | Add-Member -Force LogStart (Get-LogLines $g).Count
        Enter-World $g
        Write-Show "game $($g.N): entering the world"
    }
    foreach ($g in $Games) { Wait-InWorld $g $g.LogStart }
    Start-Sleep -Seconds 5
    foreach ($g in $Games) {
        Use-Game $g
        Invoke-Chat $g '/gamerule sendcommandfeedback false'
        Set-BeDDWindow -ClientWidth $ClientW -ClientHeight $ClientH | Out-Null
        Save-Shot $g 'in-world'
    }
    Disconnect-BeDD

    $take = 1
    $deadline = (Get-Date).AddMinutes($MaxMinutes)
    if ($KeepOpen) { Write-Show 'ready: create out\take for a take, out\stop to end'; $take = 0 } else { Show-Take $take }
    while ($KeepOpen -and (Get-Date) -lt $deadline -and -not (Test-Path (Join-Path $Out 'stop'))) {
        if (Test-Path (Join-Path $Out 'take')) {
            Remove-Item (Join-Path $Out 'take')
            $take++
            Show-Take $take
        }
        Start-Sleep -Milliseconds 500
    }
} finally {
    Disconnect-BeDD
    foreach ($g in $Games) { Reset-Instance $g }
    if ($env:BEDD_MANAGER_PIPE -eq $ManagerPipe) { & (Join-Path $Bin 'BeDDManager.ps1') -Shutdown | Out-Null; $env:BEDD_MANAGER_PIPE = $null }
    Exit-SignalingLock
    foreach ($s in $locks) { $s.Dispose() }
    Remove-Item (Join-Path $Out 'stop'), (Join-Path $Out 'go') -ErrorAction SilentlyContinue
    Write-Show 'games closed, mods restored, locks released'
}
