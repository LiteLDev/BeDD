<#
.SYNOPSIS
  Start a LeviLauncher game instance directly, without activating its window.

.DESCRIPTION
  Runs versions\<Instance>\Minecraft.Windows.exe with STARTUPINFO.wShowWindow = SW_SHOWNOACTIVATE,
  so the window appears without taking the foreground (LeviLauncher is not needed: the exe loads
  PreLoader/LeviLamina itself). LEVI_HEADLESS_RETURN_FOCUS is set to the current foreground
  window, in case the game still activates itself while starting. Returns the process.

  Starts the session's manager window first unless it runs (HeadlessManager.ps1): the game then
  shows up there instead of as a window of its own, and closing the manager closes it. -Label
  names the game there (default: the project and worktree of the calling script).
  -NoManager, or LEVI_HEADLESS_MANAGER=0, starts the game without it.

.EXAMPLE
  & bin\LeviHeadlessInput\Start-HeadlessGame.ps1 -Instance 1.26.51.01-hi2
#>
param(
    [Parameter(Mandatory)][string]$Instance,
    [string]$VersionsDir = (Join-Path $env:APPDATA 'levilauncher.exe\versions'),
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

if (-not ('HeadlessInput.Launch' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace HeadlessInput {
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

$dir = Join-Path $VersionsDir $Instance
# Always this exact name: LeviLauncher.dll's data isolation keys on it (under another name the game
# would use the default %APPDATA%\Minecraft Bedrock profile instead of the instance's data).
$exe = Join-Path $dir 'Minecraft.Windows.exe'
if (-not (Test-Path $exe)) { throw "No game instance at $dir" }
# Only a game whose mod links to the manager (background mode, "manager" not false) goes there.
$config = $null
try { $config = Get-Content (Join-Path $dir 'mods\LeviHeadlessInput\config\config.json') -Raw -ErrorAction Stop | ConvertFrom-Json } catch { }
$managed = -not $NoManager -and $env:LEVI_HEADLESS_MANAGER -ne '0' -and $config -and $config.background -eq $true -and $config.manager -ne $false
$manager = $null
if ($managed) { $manager = & (Join-Path $PSScriptRoot 'HeadlessManager.ps1') -Ensure }
if (-not $Label) { $Label = if ($env:LEVI_HEADLESS_LABEL) { $env:LEVI_HEADLESS_LABEL } else { Get-CallerLabel } }
# For the game only (the mod reads them): this process keeps its own environment.
$saved = @{ label = $env:LEVI_HEADLESS_LABEL; focus = $env:LEVI_HEADLESS_RETURN_FOCUS }
try {
    $env:LEVI_HEADLESS_LABEL = $Label
    $env:LEVI_HEADLESS_RETURN_FOCUS = [string][int64][HeadlessInput.Launch]::GetForegroundWindow()
    $id = [HeadlessInput.Launch]::StartNoActivate($exe, $dir)
} finally {
    $env:LEVI_HEADLESS_LABEL = $saved.label
    $env:LEVI_HEADLESS_RETURN_FOCUS = $saved.focus
}
# Shown in the manager from the start, also while the game loads.
if ($manager) { & (Join-Path $PSScriptRoot 'HeadlessManager.ps1') -Adopt $id -Instance $Instance -Label $Label | Out-Null }
return Get-Process -Id $id
