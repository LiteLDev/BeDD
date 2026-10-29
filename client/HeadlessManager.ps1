<#
.SYNOPSIS
  The manager window: one per logon session, it shows every headless game and closes them all
  when it is closed.

.DESCRIPTION
  Start-HeadlessGame.ps1 (and the harness shim's Start-Game) start the manager with -Ensure when
  it does not run yet, so the first test run of the session opens it and later runs reuse it. Every
  game with LeviHeadlessInput in background mode links to it: the game window then stays off
  screen, off the taskbar and out of Alt+Tab, and the manager shows a live picture of it instead.
  Many agents starting and closing games no longer make windows pop up and vanish on the desktop.

  Closing the manager window closes every game it shows (it asks first). Each game is put into a
  kill-on-close job of the manager, so the games also go when the manager is killed. Right-click a
  game for more: take it over with your own mouse and keyboard, hand it back, close it, open its
  log. Double-click a game to show only that one.

  The manager runs outside the caller's process tree (started through WMI), so the agent or
  harness that happened to start it can finish without taking the other agents' games along.
  It needs PowerShell 7 (pwsh). The client switches below also work in Windows PowerShell.

  Talks JSON lines on \\.\pipe\LeviHeadlessInput-Manager-<session id> (see CONTRIBUTING.md).
  Logs to %LOCALAPPDATA%\LeviHeadlessInput\manager.log.

.EXAMPLE
  & .\HeadlessManager.ps1 -Ensure       # start it unless it runs; returns its pipe name
  & .\HeadlessManager.ps1 -List         # the games it shows
  & .\HeadlessManager.ps1 -Shutdown     # close it and every game, without asking
#>
param(
    # Start the manager unless it already runs. Returns its pipe name ($null if it could not start).
    [switch]$Ensure,
    # Register a game process right after starting it (Start-HeadlessGame.ps1 does this).
    [int]$Adopt = 0,
    [string]$Instance = '',
    [string]$Label = '',
    # The games the manager shows.
    [switch]$List,
    # Close the manager and every game it shows, without asking.
    [switch]$Shutdown,
    # What the right-click menu does to one game: show (take it over with the real mouse and
    # keyboard), hide (hand it back to the script), close.
    [int]$Game = 0,
    [ValidateSet('show', 'hide', 'close')][string]$Action = 'show',
    [int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'

$PipeName = "LeviHeadlessInput-Manager-$([Diagnostics.Process]::GetCurrentProcess().SessionId)"

# One request, one reply (or $null when no manager answers).
function Invoke-Manager([hashtable]$Request, [int]$TimeoutMs = 2000) {
    $p = New-Object System.IO.Pipes.NamedPipeClientStream('.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $p.Connect($TimeoutMs)
        $enc = New-Object System.Text.UTF8Encoding($false)
        $w = New-Object System.IO.StreamWriter($p, $enc, 4096, $true)
        $w.NewLine = "`n"; $w.AutoFlush = $true
        $r = New-Object System.IO.StreamReader($p, $enc, $false, 4096, $true)
        $w.WriteLine(($Request | ConvertTo-Json -Compress -Depth 5))
        $t = $r.ReadLineAsync()
        if (-not $t.Wait($TimeoutMs) -or -not $t.Result) { return $null }
        return $t.Result | ConvertFrom-Json
    } catch {
        return $null
    } finally {
        $p.Dispose()
    }
}

function Test-Manager { $r = Invoke-Manager @{ op = 'ping' } 1000; return [bool]($r -and $r.ok) }

function Get-Pwsh {
    if ($PSVersionTable.PSEdition -eq 'Core') { return (Get-Process -Id $PID).Path }
    $c = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

function Start-Manager {
    if (Test-Manager) { return $PipeName }
    # Several runs may get here at once: one of them starts the manager, the others wait for it.
    $mutex = New-Object System.Threading.Mutex($false, 'Local\LeviHeadlessInput.ManagerStart')
    $owned = $false
    try {
        try { $owned = $mutex.WaitOne($TimeoutSeconds * 1000) } catch [System.Threading.AbandonedMutexException] { $owned = $true }
        if (Test-Manager) { return $PipeName }
        $pwsh = Get-Pwsh
        if (-not $pwsh) { Write-Warning '[headless] the manager window needs PowerShell 7 (pwsh); games run without it'; return $null }
        $argLine = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$PSCommandPath`""
        # Through WMI, so the manager is not part of this process tree or its job (an agent's shell
        # may close its job, and the manager would take every other agent's games with it), and
        # inside a headless console host, so no console window shows up.
        $started = $false
        try {
            $conhost = Join-Path $env:SystemRoot 'System32\conhost.exe'
            $c = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
                CommandLine = "`"$conhost`" --headless `"$pwsh`" $argLine"; CurrentDirectory = $PSScriptRoot }
            $started = $c.ReturnValue -eq 0
            if (-not $started) { Write-Warning "[headless] starting the manager through WMI failed ($($c.ReturnValue))" }
        } catch {
            Write-Warning "[headless] starting the manager through WMI failed: $($_.Exception.Message)"
        }
        if (-not $started) {
            # It then closes with this process tree's job, if there is one.
            Start-Process -FilePath $pwsh -ArgumentList $argLine -WindowStyle Hidden | Out-Null
        }
        $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
        while ((Get-Date) -lt $deadline) {
            if (Test-Manager) { return $PipeName }
            Start-Sleep -Milliseconds 250
        }
        Write-Warning "[headless] the manager window did not start within $TimeoutSeconds s (see $env:LOCALAPPDATA\LeviHeadlessInput\manager.log); games run without it"
        return $null
    } finally {
        if ($owned) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

if ($Ensure) { return Start-Manager }
if ($Adopt) {
    $r = Invoke-Manager @{ op = 'adopt'; pid = $Adopt; instance = $Instance; label = $Label }
    return [bool]($r -and $r.ok)
}
if ($List) {
    $r = Invoke-Manager @{ op = 'list' }
    if (-not $r) { return @() }
    return @($r.games)
}
if ($Game) {
    if ($Action -eq 'show') {
        # Let the game come to the front (this process may hold the foreground right now).
        if (-not ('HeadlessInput.Fg' -as [type])) {
            Add-Type -Namespace HeadlessInput -Name Fg -MemberDefinition '[DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);'
        }
        [HeadlessInput.Fg]::AllowSetForegroundWindow($Game) | Out-Null
    }
    return Invoke-Manager @{ op = 'game'; pid = $Game; action = $Action } 15000
}
if ($Shutdown) {
    $r = Invoke-Manager @{ op = 'shutdown' }
    if (-not $r) { return $false }
    # Wait until it has closed the games and gone.
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline -and (Invoke-Manager @{ op = 'ping' } 500)) { Start-Sleep -Milliseconds 250 }
    return $true
}

# --- the manager itself -----------------------------------------------------------------------------

if ($PSVersionTable.PSEdition -ne 'Core') { throw 'The manager window needs PowerShell 7 (pwsh)' }
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
# WinForms and System.Drawing with the private assemblies their public types build on.
$refs = @(Get-ChildItem (Split-Path ([System.Windows.Forms.Form].Assembly.Location)) -Filter 'System.*.dll' |
    Where-Object Name -Match '^System\.(Windows\.Forms|Drawing|Private\.Windows)' | ForEach-Object FullName)
$refs += 'System.ComponentModel.Primitives', 'System.ComponentModel', 'System.Collections', 'System.Linq',
    'System.Threading', 'System.Threading.Thread', 'System.IO.Pipes', 'System.Text.Json', 'System.Memory',
    'System.Runtime.InteropServices', 'System.Diagnostics.Process', 'System.Text.Encoding.Extensions'
Add-Type -IgnoreWarnings -ReferencedAssemblies $refs -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace LeviHeadlessInput {

static class Native {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left, Top, Right, Bottom;
        public RECT(Rectangle r) { Left = r.Left; Top = r.Top; Right = r.Right; Bottom = r.Bottom; }
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct SIZE { public int cx, cy; }
    [StructLayout(LayoutKind.Sequential)]
    public struct THUMBNAIL_PROPERTIES {
        public uint dwFlags;
        public RECT rcDestination, rcSource;
        public byte opacity;
        [MarshalAs(UnmanagedType.Bool)] public bool fVisible;
        [MarshalAs(UnmanagedType.Bool)] public bool fSourceClientAreaOnly;
    }
    public const uint DWM_TNP_RECTDESTINATION = 0x1, DWM_TNP_OPACITY = 0x4, DWM_TNP_VISIBLE = 0x8, DWM_TNP_SOURCECLIENTAREAONLY = 0x10;
    [DllImport("dwmapi.dll")] public static extern int DwmRegisterThumbnail(IntPtr dest, IntPtr src, out IntPtr thumb);
    [DllImport("dwmapi.dll")] public static extern int DwmUnregisterThumbnail(IntPtr thumb);
    [DllImport("dwmapi.dll")] public static extern int DwmUpdateThumbnailProperties(IntPtr thumb, ref THUMBNAIL_PROPERTIES props);
    [DllImport("dwmapi.dll")] public static extern int DwmQueryThumbnailSourceSize(IntPtr thumb, out SIZE size);

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    public const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;
    public const int JobObjectExtendedLimitInformation = 9;
    public const uint PROCESS_TERMINATE = 0x1, PROCESS_SET_QUOTA = 0x100;
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, uint length);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool QueryFullProcessImageNameW(IntPtr h, int flags, StringBuilder path, ref int size);
    public static string ImagePath(int pid) {
        IntPtr h = OpenProcess(0x1000, false, pid); // PROCESS_QUERY_LIMITED_INFORMATION
        if (h == IntPtr.Zero) return "";
        try {
            StringBuilder sb = new StringBuilder(1024);
            int n = sb.Capacity;
            return QueryFullProcessImageNameW(h, 0, sb, ref n) ? sb.ToString() : "";
        } finally { CloseHandle(h); }
    }

    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern int SetCurrentProcessExplicitAppUserModelID(string id);
}

// One game's link (the mod connected and said hello): request/response, one at a time.
sealed class Link : IDisposable {
    readonly NamedPipeServerStream pipe;
    readonly StreamReader reader;
    readonly StreamWriter writer;
    readonly object gate = new object();
    int nextId;
    volatile bool disposed;

    public Link(NamedPipeServerStream pipe, StreamReader reader) {
        this.pipe = pipe;
        this.reader = reader;
        writer = new StreamWriter(pipe, new UTF8Encoding(false), 4096, true);
        writer.NewLine = "\n";
        writer.AutoFlush = true;
    }

    public bool Alive { get { return !disposed && pipe.IsConnected; } }

    // The answer to the game's hello: it is linked from then on.
    public bool Welcome() {
        lock (gate) {
            try {
                writer.WriteLine(new JsonObject { ["ok"] = true }.ToJsonString());
                return true;
            } catch {
                Dispose();
                return false;
            }
        }
    }

    // The reply, or null (the link is then closed: a late reply would be taken for the next one).
    public JsonObject Call(JsonObject request, int timeoutMs) {
        lock (gate) {
            if (disposed) return null;
            try {
                request["id"] = ++nextId;
                writer.WriteLine(request.ToJsonString());
                Task<string> t = reader.ReadLineAsync();
                if (!t.Wait(timeoutMs) || t.Result == null) { Dispose(); return null; }
                return JsonNode.Parse(t.Result) as JsonObject;
            } catch {
                Dispose();
                return null;
            }
        }
    }

    public void Dispose() {
        disposed = true;
        try { pipe.Dispose(); } catch { }
    }
}

sealed class Game {
    public int Pid;
    public string Instance = "", Label = "", PipeName = "", ExePath = "";
    public Process Process;
    public IntPtr Job;
    public bool JobTried;
    public string JobNote = "";
    public Link Link;
    public bool EverLinked;
    public IntPtr Hwnd;
    public bool Background = true, Managed, TakenOver;
    public long Synthetic = -1;
    public DateTime Started = DateTime.Now, LastInput = DateTime.MinValue;
    public bool Closing;
    // UI thread only
    public IntPtr Thumb, ThumbSource;
    public Rectangle Tile, Caption, Body;
    public Size Source;

    public string Title { get { return Label.Length > 0 ? Label : (Instance.Length > 0 ? Instance : "Minecraft"); } }
}

public sealed class ManagerForm : Form {
    readonly string pipeName;
    readonly object sync = new object();
    readonly List<Game> games = new List<Game>();
    readonly System.Windows.Forms.Timer clock = new System.Windows.Forms.Timer();
    volatile bool shuttingDown;
    bool forceClose;
    int focusPid;

    static readonly string DataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LeviHeadlessInput");
    static readonly string SettingsFile = Path.Combine(DataDir, "manager.json");
    static readonly string LogFile = Path.Combine(DataDir, "manager.log");
    static readonly object logGate = new object();

    static readonly Color Back = Color.FromArgb(24, 24, 27), CaptionBack = Color.FromArgb(39, 39, 42), BodyBack = Color.Black;
    static readonly Color Text1 = Color.FromArgb(244, 244, 245), Text2 = Color.FromArgb(161, 161, 170);
    static readonly Color Active = Color.FromArgb(74, 222, 128), Idle = Color.FromArgb(113, 113, 122), Warn = Color.FromArgb(251, 191, 36), Bad = Color.FromArgb(248, 113, 113);

    public static void Log(string message) {
        try {
            lock (logGate) {
                Directory.CreateDirectory(DataDir);
                File.AppendAllText(LogFile, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + " [" + Environment.ProcessId + "] " + message + Environment.NewLine);
            }
        } catch { }
    }

    public static int Run(string pipeName) {
        try {
            Directory.CreateDirectory(DataDir);
            if (File.Exists(LogFile) && new FileInfo(LogFile).Length > (1 << 20)) File.Delete(LogFile);
        } catch { }
        // One manager per logon session. A manager that is closing still holds this for a moment:
        // wait for it rather than give up (the run that started us waits for the pipe).
        using (Mutex single = new Mutex(false, @"Local\LeviHeadlessInput.Manager")) {
            bool owned;
            try { owned = single.WaitOne(30000); } catch (AbandonedMutexException) { owned = true; }
            if (!owned) { Log("another manager runs in this session; exiting"); return 1; }
            try {
                Native.SetCurrentProcessExplicitAppUserModelID("LiteLDev.LeviHeadlessInput.Manager");
                Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
                Application.ThreadException += (s, e) => Log("UI error: " + e.Exception);
                AppDomain.CurrentDomain.UnhandledException += (s, e) => Log("fatal: " + e.ExceptionObject);
                Log("manager started on \\\\.\\pipe\\" + pipeName);
                using (ManagerForm form = new ManagerForm(pipeName)) Application.Run(form);
                Log("manager exited");
                return 0;
            } finally {
                single.ReleaseMutex();
            }
        }
    }

    ManagerForm(string pipeName) {
        this.pipeName = pipeName;
        Text = "LeviHeadlessInput";
        BackColor = Back;
        ForeColor = Text1;
        Font = new Font("Segoe UI", 9f);
        DoubleBuffered = true;
        AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.Manual;
        Icon = MakeIcon();
        MinimumSize = new Size(360, 240);
        Rectangle work = Screen.PrimaryScreen.WorkingArea;
        Bounds = LoadBounds() ?? new Rectangle(work.Left + work.Width / 5, work.Top + work.Height / 6, work.Width * 3 / 5, work.Height * 2 / 3);
        clock.Interval = 1000;
        clock.Tick += (s, e) => Invalidate();
        clock.Start();
        new Thread(AcceptLoop) { IsBackground = true, Name = "accept" }.Start();
        new Thread(PollLoop) { IsBackground = true, Name = "poll" }.Start();
    }

    // Opened by a script on someone's desktop: never take the focus from what they are doing.
    protected override bool ShowWithoutActivation { get { return true; } }

    int S(int v) { return (int)Math.Round(v * DeviceDpi / 96.0); }

    void Post(Action a) {
        try { if (IsHandleCreated && !IsDisposed) BeginInvoke(a); } catch { }
    }

    static Icon MakeIcon() {
        using (Bitmap b = new Bitmap(32, 32)) {
            using (Graphics g = Graphics.FromImage(b)) {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);
                Color[] c = { Color.FromArgb(74, 222, 128), Color.FromArgb(96, 165, 250), Color.FromArgb(251, 191, 36), Color.FromArgb(161, 161, 170) };
                for (int i = 0; i < 4; i++) using (SolidBrush br = new SolidBrush(c[i])) g.FillRectangle(br, 2 + (i % 2) * 15, 2 + (i / 2) * 15, 13, 13);
            }
            return Icon.FromHandle(b.GetHicon());
        }
    }

    // --- pipe server -------------------------------------------------------------------------------

    void AcceptLoop() {
        while (true) {
            NamedPipeServerStream s = null;
            try {
                s = new NamedPipeServerStream(pipeName, PipeDirection.InOut, NamedPipeServerStream.MaxAllowedServerInstances,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                s.WaitForConnection();
                NamedPipeServerStream client = s;
                new Thread(() => HandleClient(client)) { IsBackground = true, Name = "client" }.Start();
            } catch (Exception e) {
                Log("pipe: " + e.Message);
                try { if (s != null) s.Dispose(); } catch { }
                Thread.Sleep(500);
            }
        }
    }

    void HandleClient(NamedPipeServerStream s) {
        try {
            StreamReader reader = new StreamReader(s, new UTF8Encoding(false), false, 4096, true);
            Task<string> first = reader.ReadLineAsync();
            if (!first.Wait(5000) || first.Result == null) { s.Dispose(); return; }
            JsonObject req = JsonNode.Parse(first.Result) as JsonObject;
            string op = Str(req, "op");
            if (op == "hello") {
                if (shuttingDown) { s.Dispose(); return; } // no welcome: it links to the next manager
                Link link = new Link(s, reader);
                if (link.Welcome()) Register(Int(req, "pid"), Str(req, "instance"), Str(req, "label"), Str(req, "pipe"), link);
                return;
            }
            JsonObject reply;
            switch (op) {
            case "ping":
                lock (sync) reply = new JsonObject { ["ok"] = !shuttingDown, ["pid"] = Environment.ProcessId, ["games"] = games.Count };
                break;
            case "adopt":
                reply = new JsonObject { ["ok"] = !shuttingDown && Register(Int(req, "pid"), Str(req, "instance"), Str(req, "label"), "", null) != null };
                break;
            case "list":
                reply = new JsonObject { ["ok"] = true, ["games"] = ListGames() };
                break;
            case "game":
                reply = GameAction(Int(req, "pid"), Str(req, "action"));
                break;
            case "shutdown":
                reply = new JsonObject { ["ok"] = true };
                Post(() => { forceClose = true; Close(); });
                break;
            default:
                reply = new JsonObject { ["ok"] = false, ["error"] = "unknown op '" + op + "'" };
                break;
            }
            if (req != null && req["id"] != null) reply["id"] = req["id"].DeepClone();
            using (StreamWriter w = new StreamWriter(s, new UTF8Encoding(false), 4096, true)) {
                w.NewLine = "\n";
                w.WriteLine(reply.ToJsonString());
                w.Flush();
            }
            // Let the client read the reply before the pipe closes.
            Task<string> rest = reader.ReadLineAsync();
            rest.Wait(2000);
            s.Dispose();
        } catch (Exception e) {
            Log("client: " + e.Message);
            try { s.Dispose(); } catch { }
        }
    }

    static string Str(JsonObject o, string key) {
        try { return o != null && o[key] != null ? o[key].GetValue<string>() : ""; } catch { return ""; }
    }

    static int Int(JsonObject o, string key) {
        try { return o != null && o[key] != null ? o[key].GetValue<int>() : 0; } catch { return 0; }
    }

    static long Long(JsonNode n, long fallback) {
        try { return n != null ? n.GetValue<long>() : fallback; } catch { }
        try { return n != null ? (long)n.GetValue<ulong>() : fallback; } catch { return fallback; }
    }

    static bool Bool(JsonNode n, bool fallback) {
        try { return n != null ? n.GetValue<bool>() : fallback; } catch { return fallback; }
    }

    JsonArray ListGames() {
        JsonArray a = new JsonArray();
        lock (sync) {
            foreach (Game g in games) {
                a.Add(new JsonObject {
                    ["pid"] = g.Pid, ["instance"] = g.Instance, ["label"] = g.Label, ["pipe"] = g.PipeName,
                    ["hwnd"] = g.Hwnd.ToInt64(), ["linked"] = g.Link != null && g.Link.Alive, ["background"] = g.Background,
                    ["managed"] = g.Managed, ["takenOver"] = g.TakenOver, ["job"] = g.Job != IntPtr.Zero, ["jobNote"] = g.JobNote, ["thumbnail"] = g.Thumb != IntPtr.Zero,
                });
            }
        }
        return a;
    }

    // --- games ---------------------------------------------------------------------------------------

    Game Register(int pid, string instance, string label, string pipe, Link link) {
        if (pid <= 0) { if (link != null) link.Dispose(); return null; }
        Game g;
        bool added = false;
        lock (sync) {
            g = games.FirstOrDefault(x => x.Pid == pid);
            if (g == null) {
                Process p;
                try { p = Process.GetProcessById(pid); } catch { if (link != null) link.Dispose(); return null; }
                g = new Game { Pid = pid, Process = p };
                g.ExePath = Native.ImagePath(pid);
                games.Add(g);
                added = true;
            }
            if (instance.Length > 0) g.Instance = instance;
            if (label.Length > 0) g.Label = label;
            if (pipe.Length > 0) g.PipeName = pipe;
            if (g.Instance.Length == 0 && g.ExePath.Length > 0) g.Instance = Path.GetFileName(Path.GetDirectoryName(g.ExePath));
            if (link != null) {
                if (g.Link != null) g.Link.Dispose();
                g.Link = link;
                g.EverLinked = true;
                // The game has loaded its mods by now (see AttachJob).
                if (!g.JobTried) AttachJob(g);
            }
        }
        if (added) {
            try {
                g.Process.EnableRaisingEvents = true;
                g.Process.Exited += (s, e) => Forget(g);
            } catch { }
            Log("game " + pid + " (" + g.Instance + ") registered");
        }
        if (link != null) Log("game " + pid + " linked" + (g.Job != IntPtr.Zero ? "" : "; " + g.JobNote));
        bool exited = false;
        try { exited = g.Process.HasExited; } catch { }
        if (exited) Forget(g);
        Post(Relayout);
        return g;
    }

    // A kill-on-close job per game: the games go when the manager goes, even if it is killed. One job
    // each, because a game already in its starter's job can only join a job nested in that one.
    // Not right after the game starts but once it links (or after a minute): the start-up of the
    // game and its mod loader is left alone.
    static void AttachJob(Game g) {
        g.JobTried = true;
        IntPtr job = Native.CreateJobObjectW(IntPtr.Zero, null);
        if (job == IntPtr.Zero) { g.JobNote = "no job (" + Marshal.GetLastWin32Error() + ")"; return; }
        Native.JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = new Native.JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags = Native.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        IntPtr process = IntPtr.Zero;
        try {
            if (!Native.SetInformationJobObject(job, Native.JobObjectExtendedLimitInformation, ref info, (uint)Marshal.SizeOf(info))) {
                g.JobNote = "job setup failed (" + Marshal.GetLastWin32Error() + ")";
            } else if ((process = Native.OpenProcess(Native.PROCESS_SET_QUOTA | Native.PROCESS_TERMINATE, false, g.Pid)) == IntPtr.Zero) {
                g.JobNote = "cannot open the process (" + Marshal.GetLastWin32Error() + ")";
            } else if (!Native.AssignProcessToJobObject(job, process)) {
                g.JobNote = "cannot join a job (" + Marshal.GetLastWin32Error() + ")";
            } else {
                g.Job = job;
                return;
            }
        } finally {
            if (process != IntPtr.Zero) Native.CloseHandle(process);
        }
        Native.CloseHandle(job);
    }

    void Forget(Game g) {
        lock (sync) {
            if (!games.Remove(g)) return;
            if (g.Link != null) g.Link.Dispose();
            g.Link = null;
            if (g.Job != IntPtr.Zero) Native.CloseHandle(g.Job); // the process is gone: nothing to kill
            g.Job = IntPtr.Zero;
        }
        Log("game " + g.Pid + " (" + g.Instance + ") exited");
        Post(() => { DropThumb(g); if (focusPid == g.Pid) focusPid = 0; Relayout(); });
    }

    void PollLoop() {
        while (true) {
            Thread.Sleep(1000);
            try { PollOnce(); } catch (Exception e) { Log("poll: " + e); }
        }
    }

    void PollOnce() {
        Game[] snapshot;
        lock (sync) snapshot = games.ToArray();
        foreach (Game g in snapshot) {
            Link link = g.Link;
            lock (sync) {
                if (!g.JobTried && (DateTime.Now - g.Started).TotalSeconds > 60) {
                    AttachJob(g);
                    Log("game " + g.Pid + " not linked after a minute" + (g.Job != IntPtr.Zero ? "; in a job now" : "; " + g.JobNote));
                }
            }
            if (link == null) {
                // Not linked (the mod is not loaded yet, or not at all): its main window, if any.
                IntPtr h = IntPtr.Zero;
                try { g.Process.Refresh(); h = g.Process.MainWindowHandle; } catch { }
                lock (sync) g.Hwnd = h;
                continue;
            }
            JsonObject r = link.Call(new JsonObject { ["op"] = "state" }, 3000);
            lock (sync) {
                if (r == null || !Bool(r["ok"], false)) {
                    if (g.Link == link) { g.Link = null; link.Dispose(); Log("game " + g.Pid + ": link lost"); }
                    continue;
                }
                g.Hwnd = new IntPtr(Long(r["hwnd"], 0));
                g.Background = Bool(r["background"], true);
                g.Managed = Bool(r["managed"], false);
                g.TakenOver = Bool(r["takenOver"], false);
                long synthetic = Long(r["stats"] != null ? r["stats"]["synthetic"] : null, -1);
                if (g.Synthetic >= 0 && synthetic != g.Synthetic) g.LastInput = DateTime.Now;
                g.Synthetic = synthetic;
            }
        }
        Post(Relayout);
    }

    JsonObject GameAction(int pid, string action) {
        Game g;
        lock (sync) g = games.FirstOrDefault(x => x.Pid == pid);
        if (g == null) return new JsonObject { ["ok"] = false, ["error"] = "no game with pid " + pid };
        if (action == "close") { CloseGame(g); return new JsonObject { ["ok"] = true }; }
        if (action != "show" && action != "hide") return new JsonObject { ["ok"] = false, ["error"] = "unknown action '" + action + "'" };
        Link link = g.Link;
        if (link == null) return new JsonObject { ["ok"] = false, ["error"] = "the game is not linked" };
        if (action == "show") Native.AllowSetForegroundWindow(pid);
        JsonObject r = link.Call(new JsonObject { ["op"] = action }, 10000);
        PollSoon();
        return r ?? new JsonObject { ["ok"] = false, ["error"] = "no answer from the game" };
    }

    void PollSoon() { Task.Run(() => { try { PollOnce(); } catch { } }); }

    // Ask the game to close (its window gets WM_CLOSE), kill it if it does not.
    void CloseGame(Game g) {
        lock (sync) {
            if (g.Closing) return;
            g.Closing = true;
        }
        Log("closing game " + g.Pid + " (" + g.Instance + ")");
        Task.Run(() => {
            try {
                Link link = g.Link;
                JsonObject r = link != null ? link.Call(new JsonObject { ["op"] = "close" }, 3000) : null;
                if (r == null || !Bool(r["ok"], false)) g.Process.CloseMainWindow();
                if (!g.Process.WaitForExit(10000)) { Log("game " + g.Pid + " did not close; killing it"); g.Process.Kill(); }
            } catch (Exception e) {
                Log("closing game " + g.Pid + ": " + e.Message);
            }
        });
        Post(Invalidate);
    }

    void CloseAllGames() {
        Game[] all;
        lock (sync) all = games.ToArray();
        if (all.Length == 0) return;
        Log("closing " + all.Length + " game(s)");
        Task[] asks = all.Select(g => Task.Run(() => {
            try {
                Link link = g.Link;
                JsonObject r = link != null ? link.Call(new JsonObject { ["op"] = "close" }, 3000) : null;
                if (r == null || !Bool(r["ok"], false)) g.Process.CloseMainWindow();
            } catch { }
        })).ToArray();
        Task.WaitAll(asks, 4000);
        DateTime deadline = DateTime.Now.AddSeconds(8);
        while (DateTime.Now < deadline && all.Any(g => { try { return !g.Process.HasExited; } catch { return false; } })) Thread.Sleep(100);
        lock (sync) {
            foreach (Game g in games) {
                bool alive = false;
                try { alive = !g.Process.HasExited; } catch { }
                if (alive) Log("game " + g.Pid + " did not close in time; " + (g.Job != IntPtr.Zero ? "closing its job" : "killing it"));
                if (g.Job != IntPtr.Zero) { Native.CloseHandle(g.Job); g.Job = IntPtr.Zero; }
                else if (alive) { try { g.Process.Kill(); } catch { } }
                if (g.Link != null) g.Link.Dispose();
            }
        }
    }

    // --- window ------------------------------------------------------------------------------------

    protected override void OnFormClosing(FormClosingEventArgs e) {
        // The close button, Alt+F4, the taskbar or a WM_CLOSE from elsewhere: ask. -Shutdown and
        // Windows shutting down do not.
        if (!forceClose && (e.CloseReason == CloseReason.UserClosing || e.CloseReason == CloseReason.None)) {
            int n;
            lock (sync) n = games.Count;
            if (n > 0) {
                string what = n == 1 ? "the Minecraft game it shows" : "the " + n + " Minecraft games it shows";
                DialogResult answer = MessageBox.Show(this,
                    "Closing this window also closes " + what + ".\n\nTest runs still driving them will fail.",
                    "LeviHeadlessInput", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
                if (answer != DialogResult.OK) { e.Cancel = true; return; }
            }
        }
        shuttingDown = true;
        SaveBounds();
        Hide();
        lock (sync) foreach (Game g in games) DropThumb(g);
        CloseAllGames();
        base.OnFormClosing(e);
    }

    protected override void OnResize(EventArgs e) { base.OnResize(e); Relayout(); }

    protected override void OnDpiChanged(DpiChangedEventArgs e) { base.OnDpiChanged(e); Relayout(); }

    void DropThumb(Game g) {
        if (g.Thumb != IntPtr.Zero) Native.DwmUnregisterThumbnail(g.Thumb);
        g.Thumb = IntPtr.Zero;
        g.ThumbSource = IntPtr.Zero;
    }

    List<Game> VisibleGames() {
        List<Game> all = games.ToList();
        if (focusPid != 0 && all.Any(g => g.Pid == focusPid)) return all.Where(g => g.Pid == focusPid).ToList();
        return all;
    }

    Rectangle Header { get { return new Rectangle(0, 0, ClientSize.Width, S(34)); } }

    void Relayout() {
        if (IsDisposed || shuttingDown) return;
        lock (sync) {
            List<Game> shown = VisibleGames();
            Rectangle area = Rectangle.FromLTRB(0, Header.Bottom, ClientSize.Width, ClientSize.Height);
            int pad = S(8), cap = S(28), n = shown.Count;
            int bestCols = 1;
            double bestScale = -1;
            for (int cols = 1; cols <= Math.Max(1, n); cols++) {
                int rows = (n + cols - 1) / Math.Max(1, cols);
                double w = (area.Width - pad * (cols + 1)) / (double)cols;
                double h = (area.Height - pad * (Math.Max(1, rows) + 1)) / (double)Math.Max(1, rows) - cap;
                double scale = Math.Min(w / 16, h / 9);
                if (scale > bestScale) { bestScale = scale; bestCols = cols; }
            }
            int rowsUsed = Math.Max(1, (n + bestCols - 1) / bestCols);
            // 16:9 tiles (the games' shape), the grid centred in the window.
            int tileW = Math.Max(1, (int)(bestScale * 16)), tileH = Math.Max(1, (int)(bestScale * 9) + cap);
            int left = area.Left + Math.Max(0, (area.Width - (tileW * bestCols + pad * (bestCols - 1))) / 2) - pad;
            int top = area.Top + Math.Max(0, (area.Height - (tileH * rowsUsed + pad * (rowsUsed - 1))) / 2) - pad;
            area = new Rectangle(left, top, area.Width, area.Height);
            for (int i = 0; i < n; i++) {
                Game g = shown[i];
                int x = area.Left + pad + (i % bestCols) * (tileW + pad), y = area.Top + pad + (i / bestCols) * (tileH + pad);
                g.Tile = new Rectangle(x, y, tileW, tileH);
                g.Caption = new Rectangle(x, y, tileW, cap);
                g.Body = Rectangle.FromLTRB(x, y + cap, x + tileW, y + tileH);
            }
            foreach (Game g in games) {
                bool visible = shown.Contains(g);
                if (!visible) g.Tile = g.Caption = g.Body = Rectangle.Empty;
                UpdateThumb(g, visible);
            }
        }
        Invalidate();
    }

    void UpdateThumb(Game g, bool visible) {
        if (g.Hwnd == IntPtr.Zero || !Native.IsWindow(g.Hwnd)) { DropThumb(g); return; }
        if (g.Thumb == IntPtr.Zero || g.ThumbSource != g.Hwnd) {
            DropThumb(g);
            IntPtr t;
            if (Native.DwmRegisterThumbnail(Handle, g.Hwnd, out t) != 0) return;
            g.Thumb = t;
            g.ThumbSource = g.Hwnd;
        }
        Native.SIZE size;
        if (Native.DwmQueryThumbnailSourceSize(g.Thumb, out size) == 0 && size.cx > 0 && size.cy > 0) g.Source = new Size(size.cx, size.cy);
        Rectangle dest = Fit(g.Body, g.Source);
        Native.THUMBNAIL_PROPERTIES p = new Native.THUMBNAIL_PROPERTIES();
        p.dwFlags = Native.DWM_TNP_RECTDESTINATION | Native.DWM_TNP_OPACITY | Native.DWM_TNP_VISIBLE | Native.DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = new Native.RECT(dest);
        p.opacity = 255;
        p.fVisible = visible && dest.Width > 0 && dest.Height > 0;
        p.fSourceClientAreaOnly = true;
        Native.DwmUpdateThumbnailProperties(g.Thumb, ref p);
    }

    // The source's aspect ratio (client area, so roughly; 16:9 until known), centred in the box.
    static Rectangle Fit(Rectangle box, Size source) {
        double aspect = source.Width > 0 && source.Height > 0 ? source.Width / (double)source.Height : 16 / 9.0;
        int w = box.Width, h = (int)(box.Width / aspect);
        if (h > box.Height) { h = box.Height; w = (int)(box.Height * aspect); }
        return new Rectangle(box.Left + (box.Width - w) / 2, box.Top + (box.Height - h) / 2, Math.Max(0, w), Math.Max(0, h));
    }

    static string Ago(DateTime t) {
        TimeSpan d = DateTime.Now - t;
        if (d.TotalSeconds < 60) return (int)d.TotalSeconds + " s";
        if (d.TotalMinutes < 60) return (int)d.TotalMinutes + " min";
        return (int)d.TotalHours + " h " + d.Minutes + " min";
    }

    void Status(Game g, out string text, out Color dot) {
        if (g.Closing) { text = "closing"; dot = Bad; return; }
        if (g.Link == null) {
            if (!g.EverLinked) { text = g.Hwnd == IntPtr.Zero ? "starting" : "not linked (LeviHeadlessInput not loaded?)"; dot = Idle; return; }
            text = "link lost"; dot = Bad; return;
        }
        if (g.TakenOver || !g.Background) { text = "yours: real mouse and keyboard"; dot = Warn; return; }
        if (g.Hwnd == IntPtr.Zero) { text = "starting"; dot = Idle; return; }
        if (g.LastInput != DateTime.MinValue && (DateTime.Now - g.LastInput).TotalSeconds < 5) { text = "input"; dot = Active; return; }
        text = "idle " + Ago(g.LastInput != DateTime.MinValue ? g.LastInput : g.Started);
        dot = Idle;
    }

    protected override void OnPaint(PaintEventArgs e) {
        Graphics gr = e.Graphics;
        gr.SmoothingMode = SmoothingMode.AntiAlias;
        gr.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
        using (Font bold = new Font(Font.FontFamily, 9.5f, FontStyle.Bold))
        using (Font small = new Font(Font.FontFamily, 8.5f))
        using (SolidBrush t1 = new SolidBrush(Text1))
        using (SolidBrush t2 = new SolidBrush(Text2))
        using (SolidBrush capBrush = new SolidBrush(CaptionBack))
        using (SolidBrush bodyBrush = new SolidBrush(BodyBack)) {
            lock (sync) {
                Rectangle header = Header;
                int n = games.Count;
                string count = n == 0 ? "No headless games" : n == 1 ? "1 headless game" : n + " headless games";
                if (focusPid != 0 && n > 1) count += " (showing one: double-click it to show all)";
                TextRenderer.DrawText(gr, count, bold, new Rectangle(S(12), 0, header.Width / 2, header.Height), Text1, TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
                TextRenderer.DrawText(gr, "Closing this window closes every game in it", small, new Rectangle(header.Width / 2, 0, header.Width / 2 - S(12), header.Height), Text2, TextFormatFlags.VerticalCenter | TextFormatFlags.Right | TextFormatFlags.EndEllipsis);
                if (n == 0) {
                    Rectangle body = Rectangle.FromLTRB(0, header.Bottom, ClientSize.Width, ClientSize.Height);
                    TextRenderer.DrawText(gr, "Games started with Start-HeadlessGame.ps1 show up here.\nRight-click a game to take it over, double-click to enlarge.", Font, body, Text2,
                        TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak);
                    return;
                }
                foreach (Game g in VisibleGames()) {
                    if (g.Tile.IsEmpty) continue;
                    gr.FillRectangle(capBrush, g.Caption);
                    gr.FillRectangle(bodyBrush, g.Body);
                    string status;
                    Color dot;
                    Status(g, out status, out dot);
                    int d = S(8);
                    using (SolidBrush db = new SolidBrush(dot)) gr.FillEllipse(db, g.Caption.Left + S(10), g.Caption.Top + (g.Caption.Height - d) / 2, d, d);
                    Rectangle text = Rectangle.FromLTRB(g.Caption.Left + S(26), g.Caption.Top, g.Caption.Right - S(10), g.Caption.Bottom);
                    Size statusSize = TextRenderer.MeasureText(gr, status, small);
                    Rectangle statusBox = new Rectangle(text.Right - statusSize.Width, text.Top, statusSize.Width, text.Height);
                    TextRenderer.DrawText(gr, status, small, statusBox, Text2, TextFormatFlags.VerticalCenter | TextFormatFlags.Right);
                    string sub = (g.Label.Length > 0 && g.Instance.Length > 0 ? g.Instance + "  ·  " : "") + "pid " + g.Pid;
                    Size titleSize = TextRenderer.MeasureText(gr, g.Title, bold);
                    Rectangle titleBox = new Rectangle(text.Left, text.Top, Math.Min(titleSize.Width, Math.Max(0, statusBox.Left - text.Left - S(12))), text.Height);
                    TextRenderer.DrawText(gr, g.Title, bold, titleBox, Text1, TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
                    Rectangle subBox = Rectangle.FromLTRB(titleBox.Right + S(10), text.Top, statusBox.Left - S(12), text.Bottom);
                    if (subBox.Width > S(30)) TextRenderer.DrawText(gr, sub, small, subBox, Text2, TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
                    if (g.Thumb == IntPtr.Zero) {
                        string what = g.Closing ? "closing…" : g.Hwnd == IntPtr.Zero ? "starting…" : "";
                        TextRenderer.DrawText(gr, what, Font, g.Body, Text2, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                    }
                }
            }
        }
    }

    Game GameAt(Point p) {
        lock (sync) return VisibleGames().FirstOrDefault(g => g.Tile.Contains(p));
    }

    protected override void OnMouseDoubleClick(MouseEventArgs e) {
        base.OnMouseDoubleClick(e);
        if (e.Button != MouseButtons.Left) return;
        Game g = GameAt(e.Location);
        if (g == null) return;
        focusPid = focusPid == g.Pid ? 0 : g.Pid;
        Relayout();
    }

    protected override void OnMouseUp(MouseEventArgs e) {
        base.OnMouseUp(e);
        if (e.Button != MouseButtons.Right) return;
        Game g = GameAt(e.Location);
        if (g == null) return;
        ContextMenuStrip menu = new ContextMenuStrip();
        menu.Items.Add(focusPid == g.Pid ? "Show all games" : "Show only this game", null, (s, a) => { focusPid = focusPid == g.Pid ? 0 : g.Pid; Relayout(); });
        menu.Items.Add(new ToolStripSeparator());
        bool linked = g.Link != null;
        ToolStripItem take = menu.Items.Add("Take over with my mouse and keyboard", null, (s, a) => RunAction(g, "show"));
        take.Enabled = linked && !g.TakenOver;
        ToolStripItem back = menu.Items.Add("Hand back to the script", null, (s, a) => RunAction(g, "hide"));
        back.Enabled = linked && (g.TakenOver || !g.Background);
        menu.Items.Add(new ToolStripSeparator());
        ToolStripItem log = menu.Items.Add("Open latest.log", null, (s, a) => OpenPath(Path.Combine(Path.GetDirectoryName(g.ExePath) ?? "", "logs", "latest.log")));
        log.Enabled = g.ExePath.Length > 0;
        ToolStripItem folder = menu.Items.Add("Open instance folder", null, (s, a) => OpenPath(Path.GetDirectoryName(g.ExePath) ?? ""));
        folder.Enabled = g.ExePath.Length > 0;
        if (g.PipeName.Length > 0) menu.Items.Add("Copy pipe name (" + g.PipeName + ")", null, (s, a) => Clipboard.SetText(g.PipeName));
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Close this game", null, (s, a) => CloseGame(g)).Enabled = !g.Closing;
        menu.Closed += (s, a) => BeginInvoke(new Action(menu.Dispose));
        menu.Show(this, e.Location);
    }

    void RunAction(Game g, string action) {
        if (action == "show") Native.AllowSetForegroundWindow(g.Pid);
        Task.Run(() => {
            JsonObject r = GameAction(g.Pid, action);
            if (!Bool(r["ok"], false)) Log("game " + g.Pid + " " + action + ": " + Str(r, "error"));
        });
    }

    static void OpenPath(string path) {
        try { if (path.Length > 0) Process.Start(new ProcessStartInfo(path) { UseShellExecute = true }); } catch (Exception e) { Log("open " + path + ": " + e.Message); }
    }

    Rectangle? LoadBounds() {
        try {
            JsonObject o = JsonNode.Parse(File.ReadAllText(SettingsFile)) as JsonObject;
            Rectangle r = new Rectangle(o["x"].GetValue<int>(), o["y"].GetValue<int>(), o["w"].GetValue<int>(), o["h"].GetValue<int>());
            // Only where a screen still shows it.
            if (Screen.AllScreens.Any(s => Rectangle.Intersect(s.WorkingArea, r).Width > 100 && Rectangle.Intersect(s.WorkingArea, r).Height > 60)) return r;
        } catch { }
        return null;
    }

    void SaveBounds() {
        try {
            Rectangle r = WindowState == FormWindowState.Normal ? Bounds : RestoreBounds;
            Directory.CreateDirectory(DataDir);
            File.WriteAllText(SettingsFile, new JsonObject { ["x"] = r.X, ["y"] = r.Y, ["w"] = r.Width, ["h"] = r.Height }.ToJsonString());
        } catch { }
    }
}

}
'@
exit [LeviHeadlessInput.ManagerForm]::Run($PipeName)
