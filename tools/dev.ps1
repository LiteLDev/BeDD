<#
.SYNOPSIS
  Launch the LeviLauncher game instance with BeDD deployed and keep it running for
  manual poking through the control pipe (tools\out\dev.stop or closing the game ends it).

.DESCRIPTION
  Takes the machine-wide e2e lock (bei-e2e.lock, shared with BEI, BeetrootSkin and
  AlleviatedBackpacks), deploys bin\BeDD with a config.json, launches the game and
  watches the desktop while it runs: every change of the foreground window and of the cursor clip
  rectangle is written to tools\out\desktop-watch.log. "Minecraft" never appearing there as the
  foreground (after the launch hand-back) and the clip staying the whole desktop is the evidence
  that the game does not take the user's mouse or keyboard. Always closes the game, removes the
  mod and releases the lock.

.EXAMPLE
  pwsh -NoProfile -File tools/dev.ps1 -Trace          # in the background, then in another shell:
  Import-Module .\bin\BeDD\BeDD.psm1; Connect-BeDD; Send-BeDDKey E
#>
param(
    [switch]$Trace,
    [ValidateSet('back', 'offscreen', 'none')][string]$Placement = 'back',
    [switch]$NoBackground,
    [int]$MaxMinutes = 60
)
$ErrorActionPreference = 'Stop'

$Root         = (Resolve-Path "$PSScriptRoot/..").Path
# The game instance (set up first if this PC lacks something) and LeviLauncher.
Import-Module (Join-Path $Root 'client\BeDDSetup.psm1') -DisableNameChecking
$Instance     = Initialize-BeDDInstance
$Launcher     = Find-LeviLauncher -Download
$VersionName  = $Instance.Name
$ModsDir      = $Instance.ModsDir
$LockFile     = Join-Path (Get-LeviLauncherData) 'bei-e2e.lock'
$OutDir       = Join-Path $Root 'tools\out'
$StopFile     = Join-Path $OutDir 'dev.stop'
$RestartFile  = Join-Path $OutDir 'dev.restart'
$WatchLog     = Join-Path $OutDir 'desktop-watch.log'
$ModDest      = Join-Path $ModsDir 'BeDD'

if (-not ('BeDDDev.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace BeDDDev {
public static class Native {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClipCursor(out RECT r);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    public static string Describe(IntPtr h) {
        if (h == IntPtr.Zero) return "<none>";
        uint pid; GetWindowThreadProcessId(h, out pid);
        var t = new System.Text.StringBuilder(256); GetWindowText(h, t, 256);
        string name = "?";
        try { name = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
        return name + " (pid " + pid + ") '" + t + "'";
    }
    // "full" when the cursor may roam the whole virtual desktop, else the clip rectangle.
    public static string Clip() {
        RECT r; GetClipCursor(out r);
        int x = GetSystemMetrics(76), y = GetSystemMetrics(77), w = GetSystemMetrics(78), h = GetSystemMetrics(79);
        if (r.Left <= x && r.Top <= y && r.Right >= x + w && r.Bottom >= y + h) return "full";
        return r.Left + "," + r.Top + "-" + r.Right + "," + r.Bottom;
    }
}
}
'@
}

function Write-Dev([string]$m) { Write-Host "[dev] $m" }

# The games of this instance only: other runs may have games of their own instances going.
function Get-DevGame {
    $prefix = $Instance.Dir.TrimEnd('\') + '\'
    Get-Process -Name 'Minecraft.Windows' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) }
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
Remove-Item $StopFile -ErrorAction SilentlyContinue
$bin = Join-Path $Root 'bin\BeDD'
if (-not (Test-Path (Join-Path $bin 'BeDD.dll'))) { throw "Build first (xmake): $bin" }

$lock = $null
while (-not $lock) {
    try { $lock = [System.IO.File]::Open($LockFile, 'OpenOrCreate', 'ReadWrite', 'None') }
    catch [System.IO.IOException] { if (-not $waitLogged) { Write-Dev 'waiting for the e2e lock'; $waitLogged = $true }; Start-Sleep -Milliseconds 250 }
}
$bytes = [Text.Encoding]::UTF8.GetBytes("pid=$PID repo=$Root at=$(Get-Date -Format o)")
$lock.SetLength(0); $lock.Write($bytes, 0, $bytes.Length); $lock.Flush()
Write-Dev 'lock acquired'
try {
    if (Get-DevGame) { throw "$VersionName is already running" }
    function Start-DevGame {
        if (Test-Path $ModDest) { Remove-Item -Recurse -Force $ModDest }
        Copy-Item -Recurse $bin $ModDest
        New-Item -ItemType Directory -Force (Join-Path $ModDest 'config') | Out-Null
        # A window of its own (not the manager window): this script watches it on the desktop.
        @{ background = -not $NoBackground; placement = $Placement; clientWidth = 1600; clientHeight = 900; trace = [bool]$Trace; manager = $false } |
            ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $ModDest 'config\config.json')
        Write-Dev "deployed $bin -> $ModDest"
        # The window that has the foreground now gets it back when the game window appears.
        $env:BEDD_RETURN_FOCUS = [string][int64][BeDDDev.Native]::GetForegroundWindow()
        Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Process -FilePath $Launcher -ArgumentList "--launch=`"$VersionName`""
        Write-Dev "launched $VersionName"
    }
    function Stop-DevGame {
        Get-DevGame | ForEach-Object {
            $_.CloseMainWindow() | Out-Null
            if (-not $_.WaitForExit(10000)) { $_.Kill() }
        }
        Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
    Start-DevGame

    "# $(Get-Date -Format o) dev.ps1 started; foreground: $([BeDDDev.Native]::Describe([BeDDDev.Native]::GetForegroundWindow())); clip: $([BeDDDev.Native]::Clip())" |
        Set-Content -Encoding utf8 $WatchLog
    $deadline = (Get-Date).AddMinutes($MaxMinutes)
    $lastFg = ''; $lastClip = ''; $seenGame = $false
    while ((Get-Date) -lt $deadline -and -not (Test-Path $StopFile)) {
        if (Test-Path $RestartFile) {
            # Redeploy the (rebuilt) mod and relaunch without giving up the lock.
            Remove-Item $RestartFile
            Write-Dev 'restart requested'
            Stop-DevGame
            $seenGame = $false
            Start-DevGame
        }
        $game = Get-DevGame
        if ($game) { $seenGame = $true } elseif ($seenGame) { Write-Dev 'game exited'; break }
        $fg = [BeDDDev.Native]::Describe([BeDDDev.Native]::GetForegroundWindow())
        $clip = [BeDDDev.Native]::Clip()
        if ($fg -ne $lastFg) { "$(Get-Date -Format 'HH:mm:ss.fff') foreground: $fg" | Add-Content -Encoding utf8 $WatchLog; $lastFg = $fg }
        if ($clip -ne $lastClip) { "$(Get-Date -Format 'HH:mm:ss.fff') clip: $clip" | Add-Content -Encoding utf8 $WatchLog; $lastClip = $clip }
        Start-Sleep -Milliseconds 100
    }
} finally {
    Get-DevGame | ForEach-Object {
        $_.CloseMainWindow() | Out-Null
        if (-not $_.WaitForExit(10000)) { $_.Kill() }
    }
    Get-Process -Name 'LeviLauncher' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 10 -and (Test-Path $ModDest); $i++) {
        try { Remove-Item -Recurse -Force $ModDest -ErrorAction Stop } catch { Start-Sleep -Seconds 2 }
    }
    if (Test-Path $ModDest) { Write-Host "[dev] could not remove $ModDest" -ForegroundColor Red }
    Remove-Item $StopFile -ErrorAction SilentlyContinue
    $lock.Dispose()
    Write-Dev 'game closed, mod removed, lock released'
}
