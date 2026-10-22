# PowerShell client for LeviHeadlessInput's control pipe (\\.\pipe\LeviHeadlessInput).
#
#   Import-Module <bin>\LeviHeadlessInput\HeadlessInput.psm1
#   Connect-HeadlessInput                       # waits for the game to load the mod
#   Send-HeadlessKey E                          # press E (inventory)
#   Send-HeadlessMouse click -RX 0.5 -RY 0.72   # left click at a fraction of the client area
#   Send-HeadlessText '/give @s apple'
#
# Every input call returns once the game thread has processed the input (or throws).
# Nothing here moves the real cursor, presses real keys or changes the foreground window.

# One pipe per game instance; $env:LEVI_HEADLESS_PIPE or Connect-HeadlessInput -PipeName picks it.
$script:PipeName = if ($env:LEVI_HEADLESS_PIPE) { $env:LEVI_HEADLESS_PIPE } else { 'LeviHeadlessInput' }
$script:Pipe     = $null
$script:Reader   = $null
$script:Writer   = $null
$script:NextId   = 0

function Disconnect-HeadlessInput {
    foreach ($o in @($script:Writer, $script:Reader, $script:Pipe)) { if ($o) { try { $o.Dispose() } catch {} } }
    $script:Pipe = $null; $script:Reader = $null; $script:Writer = $null
}

# True when the game is running with the mod loaded (the pipe exists).
function Test-HeadlessInput {
    return [System.IO.Directory]::GetFiles('\\.\pipe\') -contains "\\.\pipe\$script:PipeName"
}

function Open-HeadlessPipe([int]$TimeoutMs) {
    Disconnect-HeadlessInput
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
function Set-HeadlessPipe([Parameter(Mandatory)][string]$PipeName) {
    if ($PipeName -ne $script:PipeName) { Disconnect-HeadlessInput; $script:PipeName = $PipeName }
}

function Connect-HeadlessInput([int]$TimeoutSeconds = 180, [string]$PipeName) {
    if ($PipeName) { Set-HeadlessPipe $PipeName }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $last = $null
    while ((Get-Date) -lt $deadline) {
        try {
            if (-not $script:Pipe -or -not $script:Pipe.IsConnected) { Open-HeadlessPipe 2000 }
            $r = Invoke-HeadlessInput @{ op = 'ping' } -NoThrow
            if ($r.ok -and $r.hwnd -ne 0) { return $r }
            $last = 'game window not created yet'
        } catch {
            $last = $_.Exception.Message
            Disconnect-HeadlessInput
        }
        Start-Sleep -Milliseconds 500
    }
    throw "LeviHeadlessInput did not answer within $TimeoutSeconds s ($last). Is the mod deployed and the game running?"
}

# Sends one request (hashtable) and returns the reply object. Throws when the reply is not ok,
# unless -NoThrow. Reconnects once if the pipe was closed (e.g. the game restarted).
function Invoke-HeadlessInput([Parameter(Mandatory)][hashtable]$Request, [switch]$NoThrow) {
    $req = @{} + $Request
    $req.id = ++$script:NextId
    $line = $req | ConvertTo-Json -Compress -Depth 5
    for ($attempt = 1; ; $attempt++) {
        try {
            if (-not $script:Pipe -or -not $script:Pipe.IsConnected) { Open-HeadlessPipe 5000 }
            $script:Writer.WriteLine($line)
            $resp = $script:Reader.ReadLine()
            if ($null -eq $resp) { throw 'pipe closed' }
            break
        } catch {
            Disconnect-HeadlessInput
            if ($attempt -ge 2) { throw "LeviHeadlessInput: $($Request.op) failed: $($_.Exception.Message)" }
        }
    }
    $r = $resp | ConvertFrom-Json
    if (-not $r.ok -and -not $NoThrow) { throw "LeviHeadlessInput: $($Request.op) failed: $($r.error)" }
    return $r
}

function Get-HeadlessState { Invoke-HeadlessInput @{ op = 'state' } }

# Background mode on/off at runtime (off = the game sees real input and focus again, e.g. to let a
# human take over a -KeepOpen session; Ctrl+Shift+F12 on the real keyboard does the same).
function Set-HeadlessBackground([bool]$On = $true) { Invoke-HeadlessInput @{ op = 'background'; on = $On } }

# Client-area size and placement ('back' = bottom of the Z order, 'offscreen', 'none'), never activating the window.
function Set-HeadlessWindow([int]$ClientWidth = 0, [int]$ClientHeight = 0, [string]$Placement = 'none') {
    Invoke-HeadlessInput @{ op = 'window'; clientWidth = $ClientWidth; clientHeight = $ClientHeight; placement = $Placement }
}

# Key by name ('E', 'ESC', 'ENTER', 'SHIFT', 'F3', '1', ...) or -Vk. -Action press|down|up.
function Send-HeadlessKey([string]$Key, [int]$Vk = 0, [ValidateSet('press', 'down', 'up')][string]$Action = 'press', [int]$HoldMs = 60) {
    $r = @{ op = 'key'; action = $Action; holdMs = $HoldMs }
    if ($Vk) { $r.vk = $Vk } else { $r.key = $Key }
    Invoke-HeadlessInput $r | Out-Null
}

# Types text as WM_CHAR (no key presses, so e.g. 't' does not open chat).
function Send-HeadlessText([Parameter(Mandatory)][string]$Text, [int]$DelayMs = 0) {
    Invoke-HeadlessInput @{ op = 'text'; text = $Text; delayMs = $DelayMs } | Out-Null
}

# Mouse at client pixels (-X -Y) or fractions of the client area (-RX -RY); the virtual cursor
# stays where it was when neither is given.
function Send-HeadlessMouse(
    [ValidateSet('move', 'down', 'up', 'click', 'wheel')][string]$Action = 'move',
    [double]$X = [double]::NaN, [double]$Y = [double]::NaN,
    [double]$RX = [double]::NaN, [double]$RY = [double]::NaN,
    [ValidateSet('left', 'right', 'middle')][string]$Button = 'left',
    [int]$Notches = 1, [int]$HoldMs = 60
) {
    $r = @{ op = 'mouse'; action = $Action; button = $Button; notches = $Notches; holdMs = $HoldMs }
    if (-not [double]::IsNaN($RX)) { $r.rx = $RX; $r.ry = $RY }
    elseif (-not [double]::IsNaN($X)) { $r.x = [int]$X; $r.y = [int]$Y }
    Invoke-HeadlessInput $r | Out-Null
}

# Raw (WM_INPUT) mouse, for the in-world camera and buttons while the mouse is grabbed.
function Send-HeadlessRaw(
    [ValidateSet('move', 'down', 'up', 'click', 'wheel')][string]$Action = 'move',
    [int]$Dx = 0, [int]$Dy = 0,
    [ValidateSet('left', 'right', 'middle')][string]$Button = 'left',
    [int]$Notches = 1, [int]$HoldMs = 60
) {
    Invoke-HeadlessInput @{ op = 'raw'; action = $Action; dx = $Dx; dy = $Dy; button = $Button; notches = $Notches; holdMs = $HoldMs } | Out-Null
}

# Releases every key and button the virtual state still holds down.
function Clear-HeadlessInput { Invoke-HeadlessInput @{ op = 'release' } | Out-Null }

Add-Type -AssemblyName System.Drawing
if (-not ('HeadlessInput.Win' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace HeadlessInput {
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
[HeadlessInput.Win]::SetProcessDpiAwarenessContext([IntPtr]::new(-4)) | Out-Null

# The game's client area as a System.Drawing.Bitmap (dispose it). PrintWindow(PW_RENDERFULLCONTENT)
# captures the window while it is covered by other windows.
function Get-HeadlessBitmap {
    $h = [IntPtr][int64](Invoke-HeadlessInput @{ op = 'ping' }).hwnd
    $wr = New-Object HeadlessInput.Win+RECT; [HeadlessInput.Win]::GetWindowRect($h, [ref]$wr) | Out-Null
    $cr = New-Object HeadlessInput.Win+RECT; [HeadlessInput.Win]::GetClientRect($h, [ref]$cr) | Out-Null
    $o = New-Object HeadlessInput.Win+POINT; [HeadlessInput.Win]::ClientToScreen($h, [ref]$o) | Out-Null
    $full = New-Object System.Drawing.Bitmap ($wr.Right - $wr.Left), ($wr.Bottom - $wr.Top)
    $gf = [System.Drawing.Graphics]::FromImage($full)
    $hdc = $gf.GetHdc()
    try { [HeadlessInput.Win]::PrintWindow($h, $hdc, 2) | Out-Null } finally { $gf.ReleaseHdc($hdc); $gf.Dispose() }
    $bmp = New-Object System.Drawing.Bitmap $cr.Right, $cr.Bottom
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, $cr.Right, $cr.Bottom),
            (New-Object System.Drawing.Rectangle ($o.X - $wr.Left), ($o.Y - $wr.Top), $cr.Right, $cr.Bottom), 'Pixel')
    } finally { $g.Dispose(); $full.Dispose() }
    return $bmp
}

# Saves the game's client area as PNG.
function Save-HeadlessScreenshot([Parameter(Mandatory)][string]$Path) {
    $bmp = Get-HeadlessBitmap
    try { $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $bmp.Dispose() }
    return $Path
}

Export-ModuleMember -Function Connect-HeadlessInput, Set-HeadlessPipe, Disconnect-HeadlessInput, Test-HeadlessInput, Invoke-HeadlessInput,
    Get-HeadlessState, Set-HeadlessBackground, Set-HeadlessWindow, Send-HeadlessKey, Send-HeadlessText,
    Send-HeadlessMouse, Send-HeadlessRaw, Clear-HeadlessInput, Get-HeadlessBitmap, Save-HeadlessScreenshot
