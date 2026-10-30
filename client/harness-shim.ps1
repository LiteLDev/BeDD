# Drop-in for the LeviLauncher e2e harnesses (BEI, BeetrootSkin, AlleviatedBackpacks): dot-source
# this file right AFTER the harness's lib.ps1, at script level:
#
#   . "$PSScriptRoot/lib.ps1"
#   . (Join-Path $beddDir 'harness-shim.ps1')     # $beddDir = bin\BeDD
#   Register-BeDDNative ([BsE2E.Native])          # optional: redirects the Native class's input methods
#
# It replaces the harness's input, focus, window and screenshot helpers with versions that talk to
# the BeDD mod over its control pipe, so a run never moves the real cursor, presses
# real keys or takes the foreground: the user can keep working while scenarios run. Scenarios do
# not change. The harness additionally has to deploy the mod (Install-BeDDMod after its
# own deploy, Uninstall-BeDDMod in its cleanup) and keep 'BeDD' out of the
# "move other mods aside" step. Initialize-BeDDInstance [-Slot N] (BeDDSetup.psm1) returns the game
# instance to use (Dir, ModsDir, PipeName) and sets up whatever a new PC still lacks.
#
# Functions replaced (same parameters and waits as the harness versions):
#   Focus-Game, Assert-GameForeground, Set-GameWindowSize, Move-Mouse, Click, Scroll, Press-Key,
#   Type-Text, Get-ClientBitmap (ignores -FromScreen); Start-Game and Stop-Game are wrapped (signaling
#   lock, see below).

$script:BeDDDir = $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'BeDD.psm1') -Force -DisableNameChecking -Global
# Initialize-BeDDInstance and friends: finds the game instance and sets up what this PC lacks.
Import-Module (Join-Path $PSScriptRoot 'BeDDSetup.psm1') -Force -DisableNameChecking -Global
$script:Headless = $true

if (-not (Get-Variable -Name WindowW -Scope Script -ErrorAction SilentlyContinue)) { $script:WindowW = 1600 }
if (-not (Get-Variable -Name WindowH -Scope Script -ErrorAction SilentlyContinue)) { $script:WindowH = 900 }

# --- deployment ---------------------------------------------------------------------------------

# Copies the mod into <ModsDir>\BeDD with a config.json that turns background mode
# on. Call it after the harness has deployed its own mod and moved other mods aside. -NoManager:
# the game keeps a window of its own instead of showing up in the session's manager window.
function Install-BeDDMod([Parameter(Mandatory)][string]$ModsDir, [switch]$Trace, [string]$PipeName = 'BeDD',
        [ValidateSet('back', 'offscreen', 'none')][string]$Placement = 'back', [switch]$NoManager) {
    $dest = Join-Path $ModsDir 'BeDD'
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Force (Join-Path $dest 'config') | Out-Null
    foreach ($f in 'BeDD.dll', 'manifest.json') { Copy-Item (Join-Path $script:BeDDDir $f) $dest }
    @{ background = $true; placement = $Placement; clientWidth = $script:WindowW; clientHeight = $script:WindowH; trace = [bool]$Trace; pipeName = $PipeName; manager = -not $NoManager } |
        ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $dest 'config\config.json')
    $script:BeDDNoManager = [bool]$NoManager
    # No console window flashing up as the game starts (LeviLamina's CrashLogger.exe).
    Set-BeDDCrashLogger $ModsDir
    Set-BeDDPipe $PipeName
    Write-Host "[e2e] headless input: deployed BeDD (the game will not take the mouse, keyboard or foreground)"
}

# Removes the mod again (retrying while the game still holds the DLL). A leftover would keep a
# normally started game deaf to the real mouse and keyboard, so harnesses call this in `finally`.
function Uninstall-BeDDMod([Parameter(Mandatory)][string]$ModsDir) {
    Disconnect-BeDD
    $dest = Join-Path $ModsDir 'BeDD'
    for ($i = 0; $i -lt 10 -and (Test-Path $dest); $i++) {
        try { Remove-Item -Recurse -Force $dest -ErrorAction Stop } catch { Start-Sleep -Seconds 2 }
    }
    if (Test-Path $dest) { Write-Host "[e2e] FAILED to remove $dest; delete it by hand" -ForegroundColor Red }
}

# Makes a world single-player: no "multiplayer game", no LAN, Xbox Live or platform broadcast
# (level.dat and level.dat_old). Every game instance on a machine signs in to Minecraft's
# signaling service with the same ID (same PC, same account), and a world opened for multiplayer
# registers a session there: with several instances joining at once they knock each other out
# ("An error has occurred", code "Blaze"). A single-player test world never touches it. Call it
# right after restoring a test world.
function Set-WorldSinglePlayer([Parameter(Mandatory)][string]$WorldDir) {
    $tags = [ordered]@{
        MultiplayerGame = 1; MultiplayerGameIntent = 1; LANBroadcast = 1; LANBroadcastIntent = 1   # TAG_Byte
        XBLBroadcastIntent = 3; PlatformBroadcastIntent = 3                                          # TAG_Int
    }
    foreach ($f in 'level.dat', 'level.dat_old') {
        $path = Join-Path $WorldDir $f
        if (-not (Test-Path $path)) { continue }
        $b = [IO.File]::ReadAllBytes($path)
        foreach ($name in $tags.Keys) {
            $type = [byte]$tags[$name]
            $nb = [Text.Encoding]::UTF8.GetBytes($name)
            $pat = [byte[]](@($type, [byte]($nb.Length -band 0xFF), [byte]($nb.Length -shr 8)) + $nb)
            # Bedrock level.dat: 8-byte header, then little-endian NBT.
            for ($i = 8; $i -le $b.Length - $pat.Length; $i++) {
                $hit = $true
                for ($j = 0; $j -lt $pat.Length; $j++) { if ($b[$i + $j] -ne $pat[$j]) { $hit = $false; break } }
                if (-not $hit) { continue }
                $v = $i + $pat.Length
                $len = if ($type -eq 1) { 1 } else { 4 }
                for ($k = 0; $k -lt $len; $k++) { $b[$v + $k] = 0 }
                break
            }
        }
        [IO.File]::WriteAllBytes($path, $b)
    }
}

# Safety net, run by the Start-Game wrapper: makes single-player every world of this instance whose
# level.dat was written in the last -Minutes, i.e. the test world the harness has just restored,
# even if the harness forgot Set-WorldSinglePlayer (a harness copied before it existed broke the
# other projects' parallel runs with "Blaze"). Older worlds (e.g. a human's own worlds in the
# LeviLauncher instance) are never touched. Needs the harness's $script:VersionDir.
function Set-RecentWorldsSinglePlayer([int]$Minutes = 15) {
    $versionDir = Get-Variable -Name VersionDir -Scope Script -ValueOnly -ErrorAction SilentlyContinue
    if (-not $versionDir) { return }
    $since = (Get-Date).AddMinutes(-$Minutes)
    $levels = Get-ChildItem (Join-Path $versionDir 'Minecraft Bedrock\Users\*\games\com.mojang\minecraftWorlds\*\level.dat') -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $since }
    foreach ($l in $levels) {
        Set-WorldSinglePlayer $l.DirectoryName
        Write-Host "[e2e] headless input: world '$(Split-Path $l.DirectoryName -Leaf)' set to single-player (signaling safety)"
    }
}

# --- Native class redirect ----------------------------------------------------------------------

# Scenarios also call the harness's C# helpers directly ([X.Native]::Key(vk, up),
# ::MouseButton(b, up), ::Wheel(delta), ::Unicode(c), ::MoveRelative(dx, dy)). When the class
# declares the matching public static delegate fields (KeyRedirect, MouseButtonRedirect,
# WheelRedirect, UnicodeRedirect, MoveRelativeRedirect) and its methods call them when set, this
# routes those calls through the pipe too. Fields the class lacks are skipped.
function Register-BeDDNative([Parameter(Mandatory)][type]$Type) {
    $set = {
        param([string]$Name, $Value)
        $f = $Type.GetField($Name)
        if ($f) { $f.SetValue($null, $Value); return $true }
        return $false
    }
    $n = 0
    if (& $set 'KeyRedirect' ([Action[uint16, bool]] {
                param($vk, $up) Send-BeDDKey -Vk ([int]$vk) -Action $(if ($up) { 'up' } else { 'down' }) })) { $n++ }
    if (& $set 'MouseButtonRedirect' ([Action[int, bool]] {
                param($button, $up)
                $b = switch ($button) { 1 { 'right' } 2 { 'middle' } default { 'left' } }
                Send-BeDDMouse -Action $(if ($up) { 'up' } else { 'down' }) -Button $b })) { $n++ }
    if (& $set 'WheelRedirect' ([Action[int]] { param($delta) Send-BeDDMouse -Action wheel -Notches ([int]($delta / 120)) })) { $n++ }
    if (& $set 'UnicodeRedirect' ([Action[char]] { param($c) Send-BeDDText ([string]$c) })) { $n++ }
    if (& $set 'MoveRelativeRedirect' ([Action[int, int]] { param($dx, $dy) Send-BeDDRaw -Action move -Dx $dx -Dy $dy })) { $n++ }
    Write-Host "[e2e] headless input: $n input method(s) of $($Type.FullName) redirected"
}

# --- harness function overrides -------------------------------------------------------------------

# --- signaling lock ------------------------------------------------------------------------------
#
# Every game instance on the machine signs in to Minecraft's online signaling service with the same
# ID (same PC, same account) when it starts, and each new sign-in knocks the previous one out. Runs
# that need online multiplayer (joining a server over NetherNet) therefore need every other game
# closed while they play. Machine-wide reader/writer lock, held through file handles (the OS
# releases them when a process dies, so a killed run never leaves it stuck):
#   - every headless game holds it SHARED from Start-Game to Stop-Game (automatic, see below);
#   - a run that sets $script:ExclusiveSignaling = $true (e.g. in its prelaunch script) holds it
#     EXCLUSIVE instead: new games wait, then it waits for the running ones to close.
# Files: <dir>\e2e-signaling.lock (the lock) and e2e-signaling.lock.wait (held by a waiting or
# holding exclusive run, so new shared holders queue behind it instead of starving it).
$script:SignalingLockDir = Get-LeviLauncherData
$script:SignalingLock = $null
$script:SignalingWait = $null

function Enter-SignalingLock([switch]$Exclusive, [int]$TimeoutMinutes = 120) {
    if ($script:SignalingLock) { return }
    New-Item -ItemType Directory -Force $script:SignalingLockDir | Out-Null
    $lock = Join-Path $script:SignalingLockDir 'e2e-signaling.lock'
    $wait = "$lock.wait"
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $nextReport = Get-Date
    $what = if ($Exclusive) { 'exclusive' } else { 'shared' }
    while ($true) {
        try {
            if ($Exclusive) {
                # Block new shared holders first, then wait for the current ones to close.
                if (-not $script:SignalingWait) { $script:SignalingWait = [IO.File]::Open($wait, 'OpenOrCreate', 'ReadWrite', 'None') }
                $script:SignalingLock = [IO.File]::Open($lock, 'OpenOrCreate', 'ReadWrite', 'None')
            } else {
                # Queue behind a waiting exclusive run: the .wait file is held without sharing then.
                $w = [IO.File]::Open($wait, 'OpenOrCreate', 'Read', 'ReadWrite')
                try { $script:SignalingLock = [IO.File]::Open($lock, 'OpenOrCreate', 'Read', 'Read') } finally { $w.Dispose() }
            }
            Write-Host "[e2e] signaling lock acquired ($what)"
            if ($Exclusive) {
                # Games outside the lock (harnesses without this shim, or loaded before it had
                # the lock) still sign in with the same ID: wait until no game runs at all. Called
                # before our own game starts, so any Minecraft.Windows process is someone else's.
                $reported = $false
                while (Get-Process -Name 'Minecraft.Windows' -ErrorAction SilentlyContinue) {
                    if ((Get-Date) -gt $deadline) {
                        Exit-SignalingLock
                        throw "Timed out after $TimeoutMinutes min waiting for games outside the signaling lock to close"
                    }
                    if (-not $reported) {
                        $others = (Get-Process -Name 'Minecraft.Windows' | ForEach-Object { "$($_.Id) $(Split-Path (Split-Path $_.Path) -Leaf)" }) -join ', '
                        Write-Host "[e2e] exclusive signaling: waiting for games outside the lock to close ($others)"
                        $reported = $true
                    }
                    Start-Sleep -Seconds 2
                }
            }
            return
        } catch [System.IO.IOException] {
            if ((Get-Date) -gt $deadline) {
                if ($script:SignalingWait) { $script:SignalingWait.Dispose(); $script:SignalingWait = $null }
                throw "Timed out after $TimeoutMinutes min waiting for the $what signaling lock ($lock)"
            }
            if ((Get-Date) -ge $nextReport) {
                $why = if ($Exclusive) { 'other games are still running' } else { 'a run that needs online multiplayer has the games to itself' }
                Write-Host "[e2e] waiting for the $what signaling lock: $why"
                $nextReport = (Get-Date).AddSeconds(60)
            }
            Start-Sleep -Seconds 2
        }
    }
}

function Exit-SignalingLock {
    if ($script:SignalingLock) { $script:SignalingLock.Dispose(); $script:SignalingLock = $null; Write-Host "[e2e] signaling lock released" }
    if ($script:SignalingWait) { $script:SignalingWait.Dispose(); $script:SignalingWait = $null }
}

# Start-Game: take the signaling lock (shared, or exclusive with $script:ExclusiveSignaling) for
# the game's lifetime, start the session's manager window unless it runs (BeDDManager.ps1; also
# for harnesses that start the game through the launcher), and remember where the user was so the
# mod hands the foreground back there. Stop-Game: release the lock once the game is closed.
if (Get-Command Start-Game -CommandType Function -ErrorAction SilentlyContinue) {
    $script:BeDDOriginalStartGame = ${function:Start-Game}
    function Start-Game {
        Enter-SignalingLock -Exclusive:([bool]$script:ExclusiveSignaling)
        Set-RecentWorldsSinglePlayer
        if (-not $script:BeDDNoManager -and $env:BEDD_MANAGER -ne '0') { & (Join-Path $script:BeDDDir 'BeDDManager.ps1') -Ensure | Out-Null }
        $env:BEDD_RETURN_FOCUS = [string][int64][BeDD.Foreground]::Get()
        & $script:BeDDOriginalStartGame @args
    }
}
if (Get-Command Stop-Game -CommandType Function -ErrorAction SilentlyContinue) {
    $script:BeDDOriginalStopGame = ${function:Stop-Game}
    function Stop-Game {
        try { & $script:BeDDOriginalStopGame @args } finally { Exit-SignalingLock }
    }
}
if (-not ('BeDD.Foreground' -as [type])) {
    Add-Type -TypeDefinition @'
namespace BeDD {
public static class Foreground {
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern System.IntPtr GetForegroundWindow();
    public static System.IntPtr Get() { return GetForegroundWindow(); }
}
}
'@
}

# Nothing to focus: the game believes it has the focus while it stays in the background.
function Focus-Game { }

# Input never depends on the foreground any more; only make sure the mod answers.
function Assert-GameForeground { Connect-BeDD -TimeoutSeconds 60 | Out-Null }

function Set-GameWindowSize([int]$ClientW = $script:WindowW, [int]$ClientH = $script:WindowH) {
    Connect-BeDD | Out-Null
    Set-BeDDWindow -ClientWidth $ClientW -ClientHeight $ClientH -Placement back | Out-Null
    Start-Sleep -Milliseconds 800
}

function Move-Mouse([double]$x, [double]$y, [switch]$Relative) {
    if ($Relative) { Send-BeDDMouse -Action move -RX $x -RY $y } else { Send-BeDDMouse -Action move -X $x -Y $y }
    Start-Sleep -Milliseconds 120
}

function Click([double]$x, [double]$y, [switch]$Relative, [switch]$Right) {
    Move-Mouse $x $y -Relative:$Relative
    Send-BeDDMouse -Action click -Button $(if ($Right) { 'right' } else { 'left' })
    Start-Sleep -Milliseconds 250
}

function Scroll([int]$notches) { Send-BeDDMouse -Action wheel -Notches $notches; Start-Sleep -Milliseconds 200 }

function Press-Key([string]$key, [int]$holdMs = 60) {
    Send-BeDDKey -Key $key -HoldMs $holdMs
    Start-Sleep -Milliseconds 200
}

function Type-Text([string]$text) {
    Send-BeDDText $text -DelayMs 20
    Start-Sleep -Milliseconds 200
}

# The client area via PrintWindow; -FromScreen is accepted and ignored (the game is not on top).
function Get-ClientBitmap([switch]$FromScreen) { Get-BeDDBitmap }
