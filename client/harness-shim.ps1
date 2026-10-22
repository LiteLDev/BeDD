# Drop-in for the LeviLauncher e2e harnesses (BEI, BeetrootSkin, AlleviatedBackpacks): dot-source
# this file right AFTER the harness's lib.ps1, at script level:
#
#   . "$PSScriptRoot/lib.ps1"
#   . (Join-Path $headlessDir 'harness-shim.ps1')     # $headlessDir = bin\LeviHeadlessInput
#   Register-HeadlessNative ([BsE2E.Native])          # optional: redirects the Native class's input methods
#
# It replaces the harness's input, focus, window and screenshot helpers with versions that talk to
# the LeviHeadlessInput mod over its control pipe, so a run never moves the real cursor, presses
# real keys or takes the foreground: the user can keep working while scenarios run. Scenarios do
# not change. The harness additionally has to deploy the mod (Install-HeadlessInputMod after its
# own deploy, Uninstall-HeadlessInputMod in its cleanup) and keep 'LeviHeadlessInput' out of the
# "move other mods aside" step.
#
# Functions replaced (same parameters and waits as the harness versions):
#   Focus-Game, Assert-GameForeground, Set-GameWindowSize, Move-Mouse, Click, Scroll, Press-Key,
#   Type-Text, Get-ClientBitmap (ignores -FromScreen), Start-Game (wrapped).

$script:HeadlessDir = $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'HeadlessInput.psm1') -Force -DisableNameChecking -Global
$script:Headless = $true

if (-not (Get-Variable -Name WindowW -Scope Script -ErrorAction SilentlyContinue)) { $script:WindowW = 1600 }
if (-not (Get-Variable -Name WindowH -Scope Script -ErrorAction SilentlyContinue)) { $script:WindowH = 900 }

# --- deployment ---------------------------------------------------------------------------------

# Copies the mod into <ModsDir>\LeviHeadlessInput with a config.json that turns background mode
# on. Call it after the harness has deployed its own mod and moved other mods aside.
function Install-HeadlessInputMod([Parameter(Mandatory)][string]$ModsDir, [switch]$Trace, [string]$PipeName = 'LeviHeadlessInput',
        [ValidateSet('back', 'offscreen', 'none')][string]$Placement = 'back') {
    $dest = Join-Path $ModsDir 'LeviHeadlessInput'
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Force (Join-Path $dest 'config') | Out-Null
    foreach ($f in 'LeviHeadlessInput.dll', 'manifest.json') { Copy-Item (Join-Path $script:HeadlessDir $f) $dest }
    @{ background = $true; placement = $Placement; clientWidth = $script:WindowW; clientHeight = $script:WindowH; trace = [bool]$Trace; pipeName = $PipeName } |
        ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $dest 'config\config.json')
    Set-HeadlessPipe $PipeName
    Write-Host "[e2e] headless input: deployed LeviHeadlessInput (the game will not take the mouse, keyboard or foreground)"
}

# Removes the mod again (retrying while the game still holds the DLL). A leftover would keep a
# normally started game deaf to the real mouse and keyboard, so harnesses call this in `finally`.
function Uninstall-HeadlessInputMod([Parameter(Mandatory)][string]$ModsDir) {
    Disconnect-HeadlessInput
    $dest = Join-Path $ModsDir 'LeviHeadlessInput'
    for ($i = 0; $i -lt 10 -and (Test-Path $dest); $i++) {
        try { Remove-Item -Recurse -Force $dest -ErrorAction Stop } catch { Start-Sleep -Seconds 2 }
    }
    if (Test-Path $dest) { Write-Host "[e2e] FAILED to remove $dest; delete it by hand" -ForegroundColor Red }
}

# --- Native class redirect ----------------------------------------------------------------------

# Scenarios also call the harness's C# helpers directly ([X.Native]::Key(vk, up),
# ::MouseButton(b, up), ::Wheel(delta), ::Unicode(c), ::MoveRelative(dx, dy)). When the class
# declares the matching public static delegate fields (KeyRedirect, MouseButtonRedirect,
# WheelRedirect, UnicodeRedirect, MoveRelativeRedirect) and its methods call them when set, this
# routes those calls through the pipe too. Fields the class lacks are skipped.
function Register-HeadlessNative([Parameter(Mandatory)][type]$Type) {
    $set = {
        param([string]$Name, $Value)
        $f = $Type.GetField($Name)
        if ($f) { $f.SetValue($null, $Value); return $true }
        return $false
    }
    $n = 0
    if (& $set 'KeyRedirect' ([Action[uint16, bool]] {
                param($vk, $up) Send-HeadlessKey -Vk ([int]$vk) -Action $(if ($up) { 'up' } else { 'down' }) })) { $n++ }
    if (& $set 'MouseButtonRedirect' ([Action[int, bool]] {
                param($button, $up)
                $b = switch ($button) { 1 { 'right' } 2 { 'middle' } default { 'left' } }
                Send-HeadlessMouse -Action $(if ($up) { 'up' } else { 'down' }) -Button $b })) { $n++ }
    if (& $set 'WheelRedirect' ([Action[int]] { param($delta) Send-HeadlessMouse -Action wheel -Notches ([int]($delta / 120)) })) { $n++ }
    if (& $set 'UnicodeRedirect' ([Action[char]] { param($c) Send-HeadlessText ([string]$c) })) { $n++ }
    if (& $set 'MoveRelativeRedirect' ([Action[int, int]] { param($dx, $dy) Send-HeadlessRaw -Action move -Dx $dx -Dy $dy })) { $n++ }
    Write-Host "[e2e] headless input: $n input method(s) of $($Type.FullName) redirected"
}

# --- harness function overrides -------------------------------------------------------------------

# Remember where the user was, so the mod hands the foreground back to that window when the game
# window appears (the launch itself still activates the game for a moment).
if (Get-Command Start-Game -CommandType Function -ErrorAction SilentlyContinue) {
    $script:HeadlessOriginalStartGame = ${function:Start-Game}
    function Start-Game {
        $env:LEVI_HEADLESS_RETURN_FOCUS = [string][int64][HeadlessInput.Foreground]::Get()
        & $script:HeadlessOriginalStartGame @args
    }
}
if (-not ('HeadlessInput.Foreground' -as [type])) {
    Add-Type -TypeDefinition @'
namespace HeadlessInput {
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
function Assert-GameForeground { Connect-HeadlessInput -TimeoutSeconds 60 | Out-Null }

function Set-GameWindowSize([int]$ClientW = $script:WindowW, [int]$ClientH = $script:WindowH) {
    Connect-HeadlessInput | Out-Null
    Set-HeadlessWindow -ClientWidth $ClientW -ClientHeight $ClientH -Placement back | Out-Null
    Start-Sleep -Milliseconds 800
}

function Move-Mouse([double]$x, [double]$y, [switch]$Relative) {
    if ($Relative) { Send-HeadlessMouse -Action move -RX $x -RY $y } else { Send-HeadlessMouse -Action move -X $x -Y $y }
    Start-Sleep -Milliseconds 120
}

function Click([double]$x, [double]$y, [switch]$Relative, [switch]$Right) {
    Move-Mouse $x $y -Relative:$Relative
    Send-HeadlessMouse -Action click -Button $(if ($Right) { 'right' } else { 'left' })
    Start-Sleep -Milliseconds 250
}

function Scroll([int]$notches) { Send-HeadlessMouse -Action wheel -Notches $notches; Start-Sleep -Milliseconds 200 }

function Press-Key([string]$key, [int]$holdMs = 60) {
    Send-HeadlessKey -Key $key -HoldMs $holdMs
    Start-Sleep -Milliseconds 200
}

function Type-Text([string]$text) {
    Send-HeadlessText $text -DelayMs 20
    Start-Sleep -Milliseconds 200
}

# The client area via PrintWindow; -FromScreen is accepted and ignored (the game is not on top).
function Get-ClientBitmap([switch]$FromScreen) { Get-HeadlessBitmap }
