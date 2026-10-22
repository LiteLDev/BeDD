<#
.SYNOPSIS
  Start a LeviLauncher game instance directly, without activating its window.

.DESCRIPTION
  Runs versions\<Instance>\Minecraft.Windows.exe with STARTUPINFO.wShowWindow = SW_SHOWNOACTIVATE,
  so the window appears without taking the foreground (LeviLauncher is not needed: the exe loads
  PreLoader/LeviLamina itself). LEVI_HEADLESS_RETURN_FOCUS is set to the current foreground
  window, in case the game still activates itself while starting. Returns the process.

.EXAMPLE
  & bin\LeviHeadlessInput\Start-HeadlessGame.ps1 -Instance 1.26.51.01-hi2
#>
param(
    [Parameter(Mandatory)][string]$Instance,
    [string]$VersionsDir = (Join-Path $env:APPDATA 'levilauncher.exe\versions')
)
$ErrorActionPreference = 'Stop'

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
$env:LEVI_HEADLESS_RETURN_FOCUS = [string][int64][HeadlessInput.Launch]::GetForegroundWindow()
$id = [HeadlessInput.Launch]::StartNoActivate($exe, $dir)
return Get-Process -Id $id
