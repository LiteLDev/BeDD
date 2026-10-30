<#
.SYNOPSIS
  Records the showcase video: three game instances driven in the background while a terminal on
  the same desktop keeps the focus.

.DESCRIPTION
  Takes the machine-wide e2e lock (bei-e2e.lock, shared with the other LiteLDev harnesses) and the
  locks of the clone slots it uses, prepares the clone instances versions\<base>-hi<N> (other mods
  moved aside, BeDD deployed with its own pipe, the Showcase world restored), starts
  the games without activating them, enters the world in each and stages the scene through chat
  commands.

  A take then shows a backdrop over a 1920x1080 (physical pixels) region in the middle of the
  primary screen, moves the three game windows into a 2x2 grid on it, opens Windows Terminal in the
  fourth cell running demo.ps1 (the script viewers read), records the region with ffmpeg
  (Desktop Duplication, i.e. what is really on the screen) and puts everything back. The windows
  are topmost only while a take runs.

  -KeepOpen keeps the games running after the first take: create tools\showcase\out\take for
  another take (demo.ps1 is re-read), tools\showcase\out\stop to end. Cleanup always closes the
  games, removes the mod, restores the other mods and options and releases the locks.

  Output: tools\showcase\out\take-<n>.mkv (near-lossless) and setup screenshots; encode.ps1 turns
  a take into the files in docs\public\showcase\.

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
# Game client size in the grid (16:9) and the recorded region (physical pixels).
$ClientW = 848; $ClientH = 477
$RegionW = 1920; $RegionH = 1080

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
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int size);
    // Visible frame (without the invisible resize borders DWM adds around a window).
    public static RECT Frame(IntPtr h) {
        RECT r;
        if (DwmGetWindowAttribute(h, 9, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(h, out r);
        return r;
    }
    public static IntPtr FindByTitle(string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
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
$HWND_TOPMOST = [IntPtr]::new(-1); $HWND_NOTOPMOST = [IntPtr]::new(-2)
$SWP_NOSIZE = 0x1; $SWP_NOMOVE = 0x2; $SWP_NOACTIVATE = 0x10

function Write-Show([string]$m) { Write-Host "[showcase] $m" }

# Desktop Duplication delivers no frames while the display is off or a system prompt (UAC,
# Windows Firewall) dims the screen: wait until one frame can be grabbed.
function Wait-ScreenCapture([int]$TimeoutMinutes = 120) {
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes); $told = $false
    while ($true) {
        $p = Start-Process $FFmpeg -PassThru -WindowStyle Hidden -ArgumentList @('-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'lavfi', '-i', 'ddagrab=output_idx=0:framerate=10,hwdownload,format=bgra', '-frames:v', '1', '-f', 'null', '-')
        if ($p.WaitForExit(8000)) { return }
        $p.Kill()
        if ((Get-Date) -gt $deadline) { throw 'The screen cannot be captured (display off or a system prompt open)' }
        if (-not $told) { Write-Show 'waiting for the screen to become capturable (display off or a system prompt open?)'; $told = $true }
        Start-Sleep -Seconds 10
    }
}

# --- instances ----------------------------------------------------------------------------------

$Games = @(for ($i = 0; $i -lt $Slots.Count; $i++) {
    $ver = "$BaseVersion-hi$($Slots[$i])"
    [pscustomobject]@{
        N = $i + 1; Slot = $Slots[$i]; Version = $ver
        Dir = Join-Path $VersionsDir $ver
        Pipe = "BeDD-$ver"
        Lock = Join-Path $Data "hi$($Slots[$i])-e2e.lock"
        Options = $null; Hwnd = [IntPtr]::Zero
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
    # Recorded on the real screen: the games keep their own windows (no manager window).
    Install-BeDDMod -ModsDir (Join-Path $g.Dir 'mods') -PipeName $g.Pipe -Placement offscreen -NoManager

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

function Show-Take([int]$Take) {
    $x0 = [int](([LhiShow.Win]::GetSystemMetrics(0) - $RegionW) / 2)
    $y0 = [int](([LhiShow.Win]::GetSystemMetrics(1) - $RegionH) / 2)
    $cellW = $RegionW / 2; $cellH = $RegionH / 2

    if (Test-Path $sceneFile) { . $sceneFile }
    Write-Show "take ${Take}: staging the scene"
    foreach ($g in $Games) { Set-Scene $g }
    Start-Sleep -Seconds 12   # chat fades

    Wait-ScreenCapture
    $stage = Start-Process pwsh -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'stage.ps1'),
        '-X', $x0, '-Y', $y0, '-W', $RegionW, '-H', $RegionH)
    $wt = $null
    try {
        Start-Sleep -Seconds 2
        foreach ($g in $Games) {
            Use-Game $g
            $h = [IntPtr][int64](Invoke-BeDD @{ op = 'ping' }).hwnd
            $g.Hwnd = $h
            [LhiShow.Win]::SetWindowText($h, "Minecraft - game $($g.N)") | Out-Null
            $col = ($g.N - 1) % 2; $row = [math]::Floor(($g.N - 1) / 2)
            $f = [LhiShow.Win]::Frame($h); $wr = New-Object LhiShow.Win+RECT; [LhiShow.Win]::GetWindowRect($h, [ref]$wr) | Out-Null
            $fw = $f.Right - $f.Left; $fh = $f.Bottom - $f.Top
            $x = $x0 + $col * $cellW + [int](($cellW - $fw) / 2) - ($f.Left - $wr.Left)
            $y = $y0 + $row * $cellH + [int](($cellH - $fh) / 2) - ($f.Top - $wr.Top)
            [LhiShow.Win]::SetWindowPos($h, $HWND_TOPMOST, $x, $y, 0, 0, $SWP_NOSIZE -bor $SWP_NOACTIVATE) | Out-Null
        }
        Disconnect-BeDD

        # The terminal: the window the "user" works in; it takes the focus like any app you open.
        Remove-Item (Join-Path $Out 'go') -ErrorAction SilentlyContinue
        $title = 'e2e - my-client-mod'
        $pipes = ($Games | ForEach-Object Pipe) -join ','
        $wt = Start-Process wt.exe -PassThru -ArgumentList @('-w', 'new', '--title', "`"$title`"", '--suppressApplicationTitle',
            'pwsh', '-NoLogo', '-NoProfile', '-File', "`"$(Join-Path $PSScriptRoot 'demo.ps1')`"", '-Pipes', $pipes, '-GoFile', "`"$(Join-Path $Out 'go')`"")
        $th = [IntPtr]::Zero
        for ($i = 0; $i -lt 60 -and $th -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $th = [LhiShow.Win]::FindByTitle($title) }
        if ($th -eq [IntPtr]::Zero) { throw 'Windows Terminal window not found' }
        Start-Sleep -Seconds 1
        $f = [LhiShow.Win]::Frame($th); $wr = New-Object LhiShow.Win+RECT; [LhiShow.Win]::GetWindowRect($th, [ref]$wr) | Out-Null
        $padL = $f.Left - $wr.Left; $padT = $f.Top - $wr.Top; $padR = $wr.Right - $f.Right; $padB = $wr.Bottom - $f.Bottom
        # Same visible frame as the game windows, centred in the last cell.
        $gf = [LhiShow.Win]::Frame($Games[0].Hwnd); $fw = $gf.Right - $gf.Left; $fh = $gf.Bottom - $gf.Top
        [LhiShow.Win]::SetWindowPos($th, $HWND_TOPMOST, $x0 + $cellW + [int](($cellW - $fw) / 2) - $padL, $y0 + $cellH + [int](($cellH - $fh) / 2) - $padT,
            $fw + $padL + $padR, $fh + $padT + $padB, 0) | Out-Null
        Start-Sleep -Seconds 2

        $file = Join-Path $Out "take-$Take.mkv"
        $ff = Start-Process $FFmpeg -PassThru -WindowStyle Hidden -ArgumentList @('-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'lavfi', '-i', "ddagrab=output_idx=0:framerate=30:draw_mouse=0:offset_x=${x0}:offset_y=${y0}:video_size=${RegionW}x${RegionH},hwdownload,format=bgra",
            '-t', $TakeSeconds, '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '10', '-pix_fmt', 'yuv444p', "`"$file`"")
        Start-Sleep -Seconds 2
        New-Item -ItemType File (Join-Path $Out 'go') | Out-Null
        Write-Show "take ${Take}: recording $TakeSeconds s -> $file"
        if (-not $ff.WaitForExit(($TakeSeconds + 30) * 1000)) { $ff.Kill(); throw "ffmpeg did not finish the take" }
    } finally {
        foreach ($g in $Games) {
            if ($g.Hwnd -ne [IntPtr]::Zero) {
                [LhiShow.Win]::SetWindowPos($g.Hwnd, $HWND_NOTOPMOST, -4000, 100, 0, 0, $SWP_NOSIZE -bor $SWP_NOACTIVATE) | Out-Null
            }
        }
        if ($stage -and -not $stage.HasExited) { $stage.Kill() }
        $th = [LhiShow.Win]::FindByTitle('e2e - my-client-mod')
        if ($th -ne [IntPtr]::Zero) { [LhiShow.Win]::SetWindowPos($th, $HWND_NOTOPMOST, 0, 0, 0, 0, $SWP_NOSIZE -bor $SWP_NOMOVE -bor $SWP_NOACTIVATE) | Out-Null }
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
    Wait-ScreenCapture
    $locks += Enter-Lock (Join-Path $Data 'bei-e2e.lock')
    foreach ($g in $Games) { $locks += Enter-Lock $g.Lock }
    Write-Show 'locks acquired'
    foreach ($g in $Games) { Initialize-Instance $g }
    Enter-SignalingLock

    foreach ($g in $Games) {
        & (Join-Path $Bin 'Start-BeDDGame.ps1') -Instance $g.Version -NoManager | Out-Null
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
        Set-BeDDWindow -ClientWidth $ClientW -ClientHeight $ClientH -Placement none | Out-Null
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
    Exit-SignalingLock
    foreach ($s in $locks) { $s.Dispose() }
    Remove-Item (Join-Path $Out 'stop'), (Join-Path $Out 'go') -ErrorAction SilentlyContinue
    Write-Show 'games closed, mods restored, locks released'
}
