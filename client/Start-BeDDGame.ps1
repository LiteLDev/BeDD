<#
.SYNOPSIS
  Start a LeviLauncher game instance directly, without activating its window.

.DESCRIPTION
  Runs versions\<Instance>\Minecraft.Windows.exe with STARTUPINFO.wShowWindow = SW_SHOWNOACTIVATE,
  so the window appears without taking the foreground (LeviLauncher is not needed: the exe loads
  PreLoader/LeviLamina itself). BEDD_RETURN_FOCUS is set to the current foreground
  window, in case the game still activates itself while starting. Returns the process.

  Starts the session's manager window first unless it runs (BeDDManager.ps1): the game then
  shows up there instead of as a window of its own, and closing the manager closes it. -Label
  names the game there (default: the project and worktree of the calling script).
  -NoManager, or BEDD_MANAGER=0, starts the game without it.

  The instance is made ready first when this PC lacks something (BeDDSetup.psm1): a clone
  (<base>-hi<N>, or -Slot N) is created from its base instance, LeviLamina is installed with lip,
  LeviLauncher adds its loader, and with no Minecraft 1.26.51 instance at all LeviLauncher is
  opened (downloaded if needed) for you to install the game. Without -Instance it starts the base
  instance BeDD finds (or its clone for -Slot 2 and up).

.EXAMPLE
  & bin\BeDD\Start-BeDDGame.ps1 -Instance 1.26.51.01-hi2
  & bin\BeDD\Start-BeDDGame.ps1 -Slot 2
#>
param(
    [string]$Instance,
    [int]$Slot = 1,
    [string]$VersionsDir,
    [string]$Label,
    [switch]$NoManager
)
$ErrorActionPreference = 'Stop'

# "LiftOff" for a script in a LiftOff checkout, "LiftOff (fix-x)" in its worktree .claude\worktrees\fix-x.
function Get-CallerLabel {
    $caller = Get-PSCallStack | Where-Object { $_.ScriptName -and $_.ScriptName -ne $PSCommandPath } | Select-Object -First 1
    $dir = if ($caller) { Split-Path $caller.ScriptName } else { (Get-Location).ProviderPath }
    for ($d = $dir; $d; $d = Split-Path $d) {
        if (Test-Path (Join-Path $d '.git')) {
            if ($d -match '[\\/]([^\\/]+)[\\/]\.claude[\\/]worktrees[\\/]([^\\/]+)$') { return "$($Matches[1]) ($($Matches[2]))" }
            return Split-Path $d -Leaf
        }
    }
    return Split-Path $dir -Leaf
}

if (-not ('BeDD.Launch' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace BeDD {
public static class Launch {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
        IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public static int StartNoActivate(string exe, string dir) {
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.dwFlags = 0x1;          // STARTF_USESHOWWINDOW
        si.wShowWindow = 4;        // SW_SHOWNOACTIVATE
        PROCESS_INFORMATION pi;
        if (!CreateProcessW(exe, "\"" + exe + "\"", IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, dir, ref si, out pi))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }
}
}
'@
}

Import-Module (Join-Path $PSScriptRoot 'BeDDSetup.psm1') -DisableNameChecking
$ready = Initialize-BeDDInstance -Name $Instance -Slot $Slot -VersionsDir $VersionsDir
$Instance = $ready.Name
$dir = $ready.Dir
# Always this exact name: LeviLauncher.dll's data isolation keys on it (under another name the game
# would use the default %APPDATA%\Minecraft Bedrock profile instead of the instance's data).
$exe = Join-Path $dir 'Minecraft.Windows.exe'
# Only a game whose mod links to the manager (background mode, "manager" not false) goes there.
$config = $null
try { $config = Get-Content (Join-Path $dir 'mods\BeDD\config\config.json') -Raw -ErrorAction Stop | ConvertFrom-Json } catch { }
# LeviLamina's crash logger in the game process instead of CrashLogger.exe, a console program that
# would flash a console window on the desktop as the game starts (see Set-BeDDCrashLogger in
# BeDD.psm1).
if ($config -and $config.background -eq $true) {
    $ll = Join-Path $dir 'mods\LeviLamina\config\Config.json'
    try {
        $c = Get-Content $ll -Raw -ErrorAction Stop | ConvertFrom-Json
        $cl = $c.modules.crashLogger
        if ($cl -and $cl.enabled -ne $false -and $cl.builtin -ne $true) {
            $cl | Add-Member -NotePropertyName builtin -NotePropertyValue $true -Force
            [IO.File]::WriteAllText($ll, ($c | ConvertTo-Json -Depth 32), (New-Object System.Text.UTF8Encoding($false)))
        }
    } catch [System.Management.Automation.ItemNotFoundException] {
    } catch {
        Write-Warning "[BeDD] could not switch LeviLamina to its built-in crash logger ($ll): $($_.Exception.Message)"
    }
}
$managed = -not $NoManager -and $env:BEDD_MANAGER -ne '0' -and $config -and $config.background -eq $true -and $config.manager -ne $false
$manager = $null
if ($managed) { $manager = & (Join-Path $PSScriptRoot 'BeDDManager.ps1') -Ensure }
if (-not $Label) { $Label = if ($env:BEDD_LABEL) { $env:BEDD_LABEL } else { Get-CallerLabel } }
# For the game only (the mod reads them): this process keeps its own environment.
$saved = @{ label = $env:BEDD_LABEL; focus = $env:BEDD_RETURN_FOCUS }
try {
    $env:BEDD_LABEL = $Label
    $env:BEDD_RETURN_FOCUS = [string][int64][BeDD.Launch]::GetForegroundWindow()
    $id = [BeDD.Launch]::StartNoActivate($exe, $dir)
} finally {
    $env:BEDD_LABEL = $saved.label
    $env:BEDD_RETURN_FOCUS = $saved.focus
}
# Shown in the manager from the start, also while the game loads.
if ($manager) { & (Join-Path $PSScriptRoot 'BeDDManager.ps1') -Adopt $id -Instance $Instance -Label $Label | Out-Null }
return Get-Process -Id $id
