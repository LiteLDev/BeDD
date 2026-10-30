# PowerShell client for BeDD's control pipe (\\.\pipe\BeDD).
#
#   Import-Module <bin>\BeDD\BeDD.psm1
#   Connect-BeDD                       # waits for the game to load the mod
#   Send-BeDDKey E                          # press E (inventory)
#   Send-BeDDMouse click -RX 0.5 -RY 0.72   # left click at a fraction of the client area
#   Send-BeDDText '/give @s apple'
#
# Every input call returns once the game thread has processed the input (or throws).
# Nothing here moves the real cursor, presses real keys or changes the foreground window.

# One pipe per game instance; $env:BEDD_PIPE or Connect-BeDD -PipeName picks it.
$script:PipeName = if ($env:BEDD_PIPE) { $env:BEDD_PIPE } else { 'BeDD' }
$script:Pipe     = $null
$script:Reader   = $null
$script:Writer   = $null
$script:NextId   = 0

function Disconnect-BeDD {
    foreach ($o in @($script:Writer, $script:Reader, $script:Pipe)) { if ($o) { try { $o.Dispose() } catch {} } }
    $script:Pipe = $null; $script:Reader = $null; $script:Writer = $null
}

# True when the game is running with the mod loaded (the pipe exists).
function Test-BeDD {
    return [System.IO.Directory]::GetFiles('\\.\pipe\') -contains "\\.\pipe\$script:PipeName"
}

function Open-BeDDPipe([int]$TimeoutMs) {
    Disconnect-BeDD
    $p = New-Object System.IO.Pipes.NamedPipeClientStream('.', $script:PipeName, [System.IO.Pipes.PipeDirection]::InOut)
    $p.Connect($TimeoutMs)
    $enc = New-Object System.Text.UTF8Encoding($false)
    $script:Pipe   = $p
    $script:Reader = New-Object System.IO.StreamReader($p, $enc, $false, 4096, $true)
    $script:Writer = New-Object System.IO.StreamWriter($p, $enc, 4096, $true)
    $script:Writer.AutoFlush = $true
    $script:Writer.NewLine = "`n"
}

# Waits until the mod's pipe accepts a connection and the game window exists. Returns the ping reply.
# Selects the pipe (game instance) the following calls talk to.
function Set-BeDDPipe([Parameter(Mandatory)][string]$PipeName) {
    if ($PipeName -ne $script:PipeName) { Disconnect-BeDD; $script:PipeName = $PipeName }
}

function Connect-BeDD([int]$TimeoutSeconds = 180, [string]$PipeName) {
    if ($PipeName) { Set-BeDDPipe $PipeName }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $last = $null
    while ((Get-Date) -lt $deadline) {
        try {
            if (-not $script:Pipe -or -not $script:Pipe.IsConnected) { Open-BeDDPipe 2000 }
            $r = Invoke-BeDD @{ op = 'ping' } -NoThrow
            if ($r.ok -and $r.hwnd -ne 0) { return $r }
            $last = 'game window not created yet'
        } catch {
            $last = $_.Exception.Message
            Disconnect-BeDD
        }
        Start-Sleep -Milliseconds 500
    }
    throw "BeDD did not answer within $TimeoutSeconds s ($last). Is the mod deployed and the game running?"
}

# Sends one request (hashtable) and returns the reply object. Throws when the reply is not ok,
# unless -NoThrow. Reconnects once if the pipe was closed (e.g. the game restarted).
function Invoke-BeDD([Parameter(Mandatory)][hashtable]$Request, [switch]$NoThrow) {
    $req = @{} + $Request
    $req.id = ++$script:NextId
    $line = $req | ConvertTo-Json -Compress -Depth 5
    for ($attempt = 1; ; $attempt++) {
        try {
            if (-not $script:Pipe -or -not $script:Pipe.IsConnected) { Open-BeDDPipe 5000 }
            $script:Writer.WriteLine($line)
            $resp = $script:Reader.ReadLine()
            if ($null -eq $resp) { throw 'pipe closed' }
            break
        } catch {
            Disconnect-BeDD
            if ($attempt -ge 2) { throw "BeDD: $($Request.op) failed: $($_.Exception.Message)" }
        }
    }
    $r = $resp | ConvertFrom-Json
    if (-not $r.ok -and -not $NoThrow) { throw "BeDD: $($Request.op) failed: $($r.error)" }
    return $r
}

function Get-BeDDState { Invoke-BeDD @{ op = 'state' } }

# What the client shows (topScreen, mouseGrabbed, showingMenu, ...) and the local player's pose
# (config gameHooks).
function Get-BeDDWorld { Invoke-BeDD @{ op = 'world' } }

# Turns the player to an absolute yaw/pitch in degrees (player_auth_input convention: yaw 0 = +Z,
# 90 = -X; pitch -90 = up) and returns once it is within 0.5 degrees (config gameHooks).
function Set-BeDDLook([Parameter(Mandatory)][double]$Yaw, [Parameter(Mandatory)][double]$Pitch, [int]$TimeoutMs = 1000) {
    Invoke-BeDD @{ op = 'look'; yaw = $Yaw; pitch = $Pitch; timeoutMs = $TimeoutMs }
}

# Background mode on/off at runtime (off = the game sees real input and focus again, e.g. to let a
# human take over a -KeepOpen session; Ctrl+Shift+F12 on the real keyboard does the same).
function Set-BeDDBackground([bool]$On = $true) { Invoke-BeDD @{ op = 'background'; on = $On } }

# Client-area size and placement ('back' = bottom of the Z order, 'offscreen', 'none'), never activating the window.
function Set-BeDDWindow([int]$ClientWidth = 0, [int]$ClientHeight = 0, [string]$Placement = 'none') {
    Invoke-BeDD @{ op = 'window'; clientWidth = $ClientWidth; clientHeight = $ClientHeight; placement = $Placement }
}

# Key by name ('E', 'ESC', 'ENTER', 'SHIFT', 'F3', '1', ...) or -Vk. -Action press|down|up.
function Send-BeDDKey([string]$Key, [int]$Vk = 0, [ValidateSet('press', 'down', 'up')][string]$Action = 'press', [int]$HoldMs = 60) {
    $r = @{ op = 'key'; action = $Action; holdMs = $HoldMs }
    if ($Vk) { $r.vk = $Vk } else { $r.key = $Key }
    Invoke-BeDD $r | Out-Null
}

# Types text as WM_CHAR (no key presses, so e.g. 't' does not open chat).
function Send-BeDDText([Parameter(Mandatory)][string]$Text, [int]$DelayMs = 0) {
    Invoke-BeDD @{ op = 'text'; text = $Text; delayMs = $DelayMs } | Out-Null
}

# Mouse at client pixels (-X -Y) or fractions of the client area (-RX -RY); the virtual cursor
# stays where it was when neither is given.
function Send-BeDDMouse(
    [ValidateSet('move', 'down', 'up', 'click', 'wheel')][string]$Action = 'move',
    [double]$X = [double]::NaN, [double]$Y = [double]::NaN,
    [double]$RX = [double]::NaN, [double]$RY = [double]::NaN,
    [ValidateSet('left', 'right', 'middle')][string]$Button = 'left',
    [int]$Notches = 1, [int]$HoldMs = 60
) {
    $r = @{ op = 'mouse'; action = $Action; button = $Button; notches = $Notches; holdMs = $HoldMs }
    if (-not [double]::IsNaN($RX)) { $r.rx = $RX; $r.ry = $RY }
    elseif (-not [double]::IsNaN($X)) { $r.x = [int]$X; $r.y = [int]$Y }
    Invoke-BeDD $r | Out-Null
}

# Raw (WM_INPUT) mouse, for the in-world camera and buttons while the mouse is grabbed.
function Send-BeDDRaw(
    [ValidateSet('move', 'down', 'up', 'click', 'wheel')][string]$Action = 'move',
    [int]$Dx = 0, [int]$Dy = 0,
    [ValidateSet('left', 'right', 'middle')][string]$Button = 'left',
    [int]$Notches = 1, [int]$HoldMs = 60
) {
    Invoke-BeDD @{ op = 'raw'; action = $Action; dx = $Dx; dy = $Dy; button = $Button; notches = $Notches; holdMs = $HoldMs } | Out-Null
}

# Switches the instance's LeviLamina (<ModsDir>\LeviLamina) to its built-in crash logger. By default
# LeviLamina starts CrashLogger.exe, a console program, as the game loads: the game has no console,
# so Windows opens one for it, and with Windows Terminal as the default terminal that console flashes
# on the desktop at every game start. The built-in crash logger catches crashes inside the game
# process instead (logs\crash as before). Start-BeDDGame.ps1 does the same for background games.
function Set-BeDDCrashLogger([Parameter(Mandatory)][string]$ModsDir) {
    $file = Join-Path $ModsDir 'LeviLamina\config\Config.json'
    if (-not (Test-Path $file)) { return } # LeviLamina writes it on the game's first start
    try {
        $c = Get-Content $file -Raw | ConvertFrom-Json
        $cl = $c.modules.crashLogger
        if (-not $cl -or $cl.enabled -eq $false -or $cl.builtin -eq $true) { return }
        $cl | Add-Member -NotePropertyName builtin -NotePropertyValue $true -Force
        [IO.File]::WriteAllText($file, ($c | ConvertTo-Json -Depth 32), (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        Write-Warning "[BeDD] could not switch LeviLamina to its built-in crash logger ($file): $($_.Exception.Message)"
    }
}

# Releases every key and button the virtual state still holds down.
function Clear-BeDDInput { Invoke-BeDD @{ op = 'release' } | Out-Null }

Add-Type -AssemblyName System.Drawing
if (-not ('BeDD.Win' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace BeDD {
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
}
}
'@
}

# Physical pixels (the desktop may use display scaling): per-monitor-v2 DPI awareness, as the harnesses do.
[BeDD.Win]::SetProcessDpiAwarenessContext([IntPtr]::new(-4)) | Out-Null

# The game's client area as a System.Drawing.Bitmap (dispose it). PrintWindow(PW_RENDERFULLCONTENT)
# captures the window while it is covered by other windows.
function Get-BeDDBitmap {
    $h = [IntPtr][int64](Invoke-BeDD @{ op = 'ping' }).hwnd
    $wr = New-Object BeDD.Win+RECT; [BeDD.Win]::GetWindowRect($h, [ref]$wr) | Out-Null
    $cr = New-Object BeDD.Win+RECT; [BeDD.Win]::GetClientRect($h, [ref]$cr) | Out-Null
    $o = New-Object BeDD.Win+POINT; [BeDD.Win]::ClientToScreen($h, [ref]$o) | Out-Null
    $full = New-Object System.Drawing.Bitmap ($wr.Right - $wr.Left), ($wr.Bottom - $wr.Top)
    $gf = [System.Drawing.Graphics]::FromImage($full)
    $hdc = $gf.GetHdc()
    try { [BeDD.Win]::PrintWindow($h, $hdc, 2) | Out-Null } finally { $gf.ReleaseHdc($hdc); $gf.Dispose() }
    $bmp = New-Object System.Drawing.Bitmap $cr.Right, $cr.Bottom
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, $cr.Right, $cr.Bottom),
            (New-Object System.Drawing.Rectangle ($o.X - $wr.Left), ($o.Y - $wr.Top), $cr.Right, $cr.Bottom), 'Pixel')
    } finally { $g.Dispose(); $full.Dispose() }
    return $bmp
}

# Saves the game's client area as PNG.
function Save-BeDDScreenshot([Parameter(Mandatory)][string]$Path) {
    $bmp = Get-BeDDBitmap
    try { $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $bmp.Dispose() }
    return $Path
}

Export-ModuleMember -Function Connect-BeDD, Set-BeDDPipe, Disconnect-BeDD, Test-BeDD, Invoke-BeDD,
    Set-BeDDCrashLogger, Get-BeDDState, Set-BeDDBackground, Set-BeDDWindow, Send-BeDDKey, Send-BeDDText,
    Send-BeDDMouse, Send-BeDDRaw, Clear-BeDDInput, Get-BeDDBitmap, Save-BeDDScreenshot,
    Get-BeDDWorld, Set-BeDDLook
