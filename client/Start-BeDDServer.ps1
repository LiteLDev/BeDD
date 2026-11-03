<#
.SYNOPSIS
  Start (or stop) a Bedrock Dedicated Server with BeDD in the background, for client + server tests.

.DESCRIPTION
  Starts <servers>\<Name>\bedrock_server_mod.exe (the BDS with LeviLamina, made ready first with
  Initialize-BeDDServer from BeDDSetup.psm1, which takes the same -Name and -ServersDir) with no
  window at all: nothing shows up on the desktop or takes the foreground.

  The server must have the BeDD server module with loopbackOnly on (the default): it binds the
  server to 127.0.0.1, so Windows Firewall never asks to allow it. Otherwise this script refuses to
  start it, unless -AllowFirewallPrompt. A small hidden PowerShell process owns the server's
  console: it writes everything the server prints to logs\console.log in the server folder and
  passes lines written to the pipe \\.\pipe\BeDD-BDS-<Name> to the server console (-Command, -Stop).

  Waits until the server is up ("Server started" in the log) and, when the server has the BeDD
  module (plugins\BeDD), until its control pipe exists. Returns an object with Process, PipeName
  ($null without the module), Port, PortV6, Dir and LogPath. A server already running is returned
  as it is.

  -Stop sends "stop" to the server console, which saves the world and exits, and kills the server
  when it has not exited after -StopTimeoutSeconds. -Command sends any other console command.

.EXAMPLE
  Import-Module bin\BeDD\BeDDSetup.psm1
  Initialize-BeDDServer -Name s1 -ModuleDir bin\BeDD-server | Out-Null
  $s = & bin\BeDD\Start-BeDDServer.ps1 -Name s1
  Connect-BeDD -PipeName $s.PipeName              # BeDD.psm1
  & bin\BeDD\Start-BeDDServer.ps1 -Name s1 -Command 'time set day'
  & bin\BeDD\Start-BeDDServer.ps1 -Name s1 -Stop
#>
param(
    [string]$Name = 'server',
    [string]$ServersDir,
    [switch]$Stop,
    [string]$Command,
    [int]$TimeoutSeconds = 180,
    [int]$StopTimeoutSeconds = 30,
    # Start a server without BeDD's loopback binding (no module, or loopbackOnly off): it listens on
    # the network, and Windows Firewall asks on the desktop the first time.
    [switch]$AllowFirewallPrompt,
    # Internal: run as the hidden process that owns the server console.
    [switch]$ConsoleHost
)
$ErrorActionPreference = 'Stop'

if (-not ('BeDD.ServerHost' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
namespace BeDD {
public static class ServerHost {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
        IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    // A process with no console window and none of this process's handles (a caller whose output
    // is captured, e.g. by CI, is not kept waiting for the server to exit). Returns its id.
    public static int StartDetached(string exe, string args, string dir) {
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        PROCESS_INFORMATION pi;
        const uint CREATE_NO_WINDOW = 0x08000000;
        if (!CreateProcessW(exe, new StringBuilder("\"" + exe + "\" " + args), IntPtr.Zero, IntPtr.Zero, false,
                CREATE_NO_WINDOW, IntPtr.Zero, dir, ref si, out pi))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }

    static readonly Regex Ansi = new Regex("\x1b\\[[0-9;?]*[A-Za-z]");

    // Runs the server with its console redirected: output (colours stripped) to logPath, lines
    // from \\.\pipe\<pipeName> to its stdin. Returns the server's exit code once it exits.
    public static int Run(string exe, string dir, string logPath, string pipeName) {
        var log = new StreamWriter(new FileStream(logPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete),
            new UTF8Encoding(false));
        log.AutoFlush = true;
        object gate = new object();
        Action<string> write = line => { if (line != null) lock (gate) log.WriteLine(Ansi.Replace(line, "")); };
        var psi = new ProcessStartInfo(exe) {
            WorkingDirectory = dir, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        var p = new Process { StartInfo = psi };
        p.OutputDataReceived += (s, e) => write(e.Data);
        p.ErrorDataReceived += (s, e) => write(e.Data);
        p.Start();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        var stdin = p.StandardInput;
        stdin.AutoFlush = true;
        var console = new Thread(() => {
            while (!p.HasExited) {
                try {
                    using (var pipe = new NamedPipeServerStream(pipeName, PipeDirection.In, 1))
                    using (var reader = new StreamReader(pipe, new UTF8Encoding(false))) {
                        pipe.WaitForConnection();
                        string line;
                        while ((line = reader.ReadLine()) != null) {
                            write("> " + line);
                            lock (gate) stdin.WriteLine(line);
                        }
                    }
                } catch (Exception e) {
                    write("[BeDD] console pipe: " + e.Message);
                    Thread.Sleep(1000);
                }
            }
        });
        console.IsBackground = true;
        console.Start();
        p.WaitForExit();
        write("[BeDD] server exited with code " + p.ExitCode);
        log.Dispose();
        return p.ExitCode;
    }
}
}
'@
}

Import-Module (Join-Path $PSScriptRoot 'BeDDSetup.psm1') -DisableNameChecking
$server = Get-BeDDServer $Name -ServersDir $ServersDir
$exe = $server.Exe

if ($ConsoleHost) {
    exit [BeDD.ServerHost]::Run($exe, $server.Dir, $server.LogPath, $server.ConsolePipe)
}

# Writes one line to the server console through its console host.
function Send-ServerConsole([string]$Line) {
    $p = New-Object System.IO.Pipes.NamedPipeClientStream('.', $server.ConsolePipe, [System.IO.Pipes.PipeDirection]::Out)
    try {
        $p.Connect(5000)
        $w = New-Object System.IO.StreamWriter($p, (New-Object System.Text.UTF8Encoding($false)))
        $w.NewLine = "`n"
        $w.WriteLine($Line)
        $w.Flush()
    } finally {
        $p.Dispose()
    }
}

# The object this script returns for a running server.
function Get-ServerInfo($Process) {
    [pscustomobject]@{
        Name     = $server.Name
        Process  = $Process
        PipeName = if ($server.Problems -contains 'bedd') { $null } else { $server.PipeName }
        Port     = $server.Port
        PortV6   = $server.PortV6
        Dir      = $server.Dir
        LogPath  = $server.LogPath
    }
}

$running = Get-BeDDServerProcess $server.Dir | Select-Object -First 1

if ($Stop) {
    if (-not $running) { Write-Host "[BeDD] server $Name is not running"; return }
    Write-Host "[BeDD] stopping server $Name (pid $($running.Id))"
    try { Send-ServerConsole 'stop' } catch { Write-Warning "[BeDD] could not reach the console of server ${Name}: $($_.Exception.Message)" }
    if (-not $running.WaitForExit($StopTimeoutSeconds * 1000)) {
        Write-Warning "[BeDD] server $Name did not stop within $StopTimeoutSeconds s: killing it"
        Stop-Process -Id $running.Id -Force -ErrorAction SilentlyContinue
        $running.WaitForExit(10000) | Out-Null
    }
    Write-Host "[BeDD] server $Name stopped"
    return
}

if ($Command) {
    if (-not $running) { throw "[BeDD] server $Name is not running" }
    Send-ServerConsole $Command
    return
}

if ($running) {
    Write-Host "[BeDD] server $Name is already running (pid $($running.Id), port $($server.Port))"
    return Get-ServerInfo $running
}

if ($server.Problems -contains 'missing' -or $server.Problems -contains 'leviLamina') {
    throw "[BeDD] server $Name is not set up in $($server.Dir) ($($server.Problems -join ', ')): run Initialize-BeDDServer -Name $Name first (BeDDSetup.psm1)"
}
# A BDS listens on every interface, and Windows Firewall then asks "allow access?" on the desktop,
# once per server folder. BeDD's server module binds it to the loopback address instead (config
# loopbackOnly, default on), so no server starts without it unless -AllowFirewallPrompt.
if (-not $AllowFirewallPrompt) {
    $why = if ($server.Problems -contains 'bedd') { 'has no BeDD server module (Initialize-BeDDServer -ModuleDir bin\BeDD-server)' }
           elseif ($server.ModulePlatform -and $server.ModulePlatform -ne 'server') { "has a BeDD module for '$($server.ModulePlatform)' in $($server.ModuleDir), not the server one (bin\BeDD-server)" }
           elseif (-not $server.ModuleLoads) { "has its BeDD module in $($server.ModuleDir), a folder not named after the mod, which LeviLamina does not load (Initialize-BeDDServer -ModuleDir moves it)" }
           elseif (-not $server.LoopbackOnly) { "has loopbackOnly: false in $($server.ModuleDir)\config\config.json" }
    if ($why) {
        throw "[BeDD] not starting server ${Name}: it $why, so it would listen on the network and Windows Firewall would ask to allow it on the desktop. Pass -AllowFirewallPrompt to start it anyway."
    }
} elseif ($server.Problems -contains 'bedd') {
    Write-Host "[BeDD] server $Name has no BeDD module: starting it without BeDD (-AllowFirewallPrompt)"
}

New-Item -ItemType Directory -Force (Split-Path $server.LogPath) | Out-Null
Remove-Item -Force $server.LogPath -ErrorAction SilentlyContinue
# The console host runs in this PowerShell, or pwsh / Windows PowerShell when this one is hosted
# elsewhere (an editor, an app).
$shell = (Get-Process -Id $PID).Path
if ((Split-Path $shell -Leaf) -notin 'pwsh.exe', 'powershell.exe') {
    $c = Get-Command pwsh, powershell -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $c) { throw '[BeDD] no pwsh.exe or powershell.exe to run the server console in' }
    $shell = $c.Source
}
$hostArgs = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Name `"$Name`" -ServersDir `"$($server.ServersDir.TrimEnd('\'))`" -ConsoleHost"
$hostId = [BeDD.ServerHost]::StartDetached($shell, $hostArgs, $server.Dir)
Write-Host "[BeDD] starting server $Name on port $($server.Port) (log: $($server.LogPath))"

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$process = $null
$started = $false
while ((Get-Date) -lt $deadline) {
    if (-not $process) { $process = Get-BeDDServerProcess $server.Dir | Select-Object -First 1 }
    $text = if (Test-Path $server.LogPath) { Get-Content $server.LogPath -Raw -ErrorAction SilentlyContinue } else { '' }
    if ($text -match 'Server started') { $started = $true; break }
    # LeviLamina did not load the module after all: stop the server before it opens its network.
    if (-not $AllowFirewallPrompt -and $text -match 'ERR \[LeviLamina\][^\r\n]*BeDD') {
        Get-BeDDServerProcess $server.Dir | Stop-Process -Force -ErrorAction SilentlyContinue
        throw "[BeDD] LeviLamina did not load BeDD in server ${Name}: stopped it before it listens on the network. See $($server.LogPath):`n$(($text -split '\r?\n' | Select-String 'ERR \[LeviLamina\]') -join "`n")"
    }
    if (-not (Get-Process -Id $hostId -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $started) {
    $tail = if (Test-Path $server.LogPath) { (Get-Content $server.LogPath -Tail 20) -join "`n" } else { '(no log)' }
    # Nothing left behind: the server (its console host exits with it), or the host when the
    # server never showed up.
    Get-BeDDServerProcess $server.Dir | Stop-Process -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $hostId -Force -ErrorAction SilentlyContinue
    throw "[BeDD] server $Name did not start within $TimeoutSeconds s. Last lines of $($server.LogPath):`n$tail"
}

if (-not $process) { $process = Get-BeDDServerProcess $server.Dir | Select-Object -First 1 }
$info = Get-ServerInfo $process
if ($info.PipeName) {
    while (-not ([IO.Directory]::GetFiles('\\.\pipe\') -contains "\\.\pipe\$($info.PipeName)")) {
        if ((Get-Date) -gt $deadline) { throw "[BeDD] server $Name is up but BeDD's pipe $($info.PipeName) did not appear (see $($server.LogPath))" }
        Start-Sleep -Milliseconds 500
    }
}
Write-Host "[BeDD] server $Name is up on port $($server.Port)$(if ($info.PipeName) { ", BeDD pipe $($info.PipeName)" })"
return $info
