using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using BeDD.Cli;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.System.Threading;

namespace BeDD.Core;

// What "launch" asks for. Dir is the instance folder (it holds Minecraft.Windows.exe).
public sealed record LaunchOptions
{
    public required string Dir { get; init; }
    public required string Instance { get; init; }
    public required string Label { get; init; }
    public bool ExclusiveSignaling { get; init; }
    // Make sure the manager window runs first (it holds Launcher.ManagerMutex).
    public bool Manager { get; init; } = true;
    // Return once the process is created instead of waiting for its discovery file.
    public bool Detach { get; init; }
    public TimeSpan Timeout { get; init; } = TimeSpan.FromMinutes(3);
    public TimeSpan LockTimeout { get; init; } = TimeSpan.FromHours(2);
}

// A started game; Endpoint is null with Detach.
public sealed record LaunchResult(int Pid, string Instance, string InstanceDir, string Label, GameEndpoint? Endpoint);

// A Minecraft.Windows process and its exe path (empty when it cannot be read).
public sealed record GameProcess(int Pid, string Exe, int SessionId = -1);

// Starts games the way Start-BeDDGame.ps1 did: Minecraft.Windows.exe of the instance folder, never
// activated (SW_SHOWNOACTIVATE), outside the caller's job, with BEDD_LABEL and BEDD_RETURN_FOCUS in
// its environment only, and holding the signaling lock through an inherited handle.
public static class Launcher
{
    // Held by the manager window while it runs (one per logon session and BeDD home).
    public static string ManagerMutex => Gui.ManagerInstance.MutexName;

    // Always this exact name: LeviLauncher.dll's data isolation keys on it (under another name the
    // game would use the default %APPDATA%\Minecraft Bedrock profile instead of the instance's data).
    public const string GameExe = "Minecraft.Windows.exe";
    public const string GameProcessName = "Minecraft.Windows";

    private const uint ErrorAccessDenied = 5;
    private const int ErrorInvalidParameter = 87;
    private const nuint ProcThreadAttributeHandleList = 0x20002;
    private const ushort SwShowNoActivate = 4;

    public static LaunchResult Launch(LaunchOptions o, Action<string>? progress = null, CancellationToken cancel = default)
    {
        string dir = Path.GetFullPath(o.Dir);
        string exe = Path.Combine(dir, GameExe);
        if (!File.Exists(exe))
            throw CliError.NotFound("ERR_GAME_NOT_FOUND", $"{o.Instance} has no {GameExe} ({dir})");
        if (!File.Exists(Path.Combine(dir, "mods", "BeDD", "BeDD.dll")))
            throw CliError.Failure("ERR_MOD_MISSING",
                $"{o.Instance} has no mods\\BeDD\\BeDD.dll: a game without it would show a window of its own",
                $"{Dispatcher.ProgramName} mod install {o.Instance}");
        // One launch per instance folder at a time: held from the already-running check until the
        // game process exists (then the check sees it).
        using var launching = LaunchLock(dir, o.Instance);
        ThrowIfRunning(dir, o.Instance);

        if (o.Manager)
            EnsureManager(TimeSpan.FromSeconds(10), progress);

        SignalingLock signaling;
        try
        {
            // Games of this logon session that do not hold the lock (another session's games sign
            // in as another user).
            int session = Process.GetCurrentProcess().SessionId;
            signaling = SignalingLock.Acquire(o.ExclusiveSignaling, o.LockTimeout, progress,
                othersRunning: () => RunningGames().Where(g => g.SessionId == session)
                    .Select(g => $"{g.Pid} {InstanceOf(g.Exe)}").ToList(), cancel: cancel);
        }
        catch (TimeoutException e)
        {
            throw CliError.Failure("ERR_SIGNALING_LOCK", e.Message, $"{Dispatcher.ProgramName} ps --all");
        }

        int pid;
        SafeProcessHandle process;
        using (signaling)
        {
            ThrowIfRunning(dir, o.Instance); // started by other means while this waited for the lock
            var env = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["BEDD_LABEL"] = o.Label,
                ["BEDD_RETURN_FOCUS"] = ((nint)PInvoke.GetForegroundWindow()).ToString(CultureInfo.InvariantCulture),
            };
            (pid, process) = Start(exe, null, dir, env, [signaling.Handle], detached: false);
            progress?.Invoke($"started {o.Instance} (pid {pid}, {(signaling.Exclusive ? "exclusive" : "shared")} signaling lock)");
        }

        using (process)
        {
            if (o.Detach)
                return new LaunchResult(pid, o.Instance, dir, o.Label, null);
            var ep = WaitForEndpoint(pid, process, dir, o.Timeout, progress, cancel);
            return new LaunchResult(pid, o.Instance, dir, o.Label, ep);
        }
    }

    private static void ThrowIfRunning(string dir, string instance)
    {
        var running = GamesOf(dir);
        if (running.Count > 0)
            throw CliError.Conflict("ERR_ALREADY_RUNNING",
                $"a game of {instance} already runs (pid {string.Join(", ", running.Select(g => g.Pid))})",
                $"{Dispatcher.ProgramName} stop {instance}");
    }

    // locks\launch-<hash of the folder>.lock, held without sharing; a second launch of the same
    // folder fails at once.
    private static SafeFileHandle LaunchLock(string dir, string instance)
    {
        Directory.CreateDirectory(Paths.Locks);
        string key = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(
            Encoding.UTF8.GetBytes(dir.TrimEnd('\\', '/').ToUpperInvariant())))[..16];
        try
        {
            return File.OpenHandle(Path.Combine(Paths.Locks, $"launch-{key}.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite,
                FileShare.None);
        }
        catch (IOException)
        {
            throw CliError.Conflict("ERR_ALREADY_RUNNING", $"{instance} is being launched by another command",
                $"{Dispatcher.ProgramName} ps --all");
        }
    }

    // Waits for run\<pid>.json; fails with the tail of the instance's newest log when the process
    // dies first.
    public static GameEndpoint WaitForEndpoint(int pid, SafeProcessHandle process, string dir, TimeSpan timeout,
        Action<string>? progress = null, CancellationToken cancel = default, string? runDir = null)
    {
        string file = Path.Combine(runDir ?? Paths.Run, pid.ToString(CultureInfo.InvariantCulture) + ".json");
        var deadline = DateTime.UtcNow + timeout;
        progress?.Invoke($"waiting for pid {pid} to announce its WebSocket endpoint ({file})");
        while (true)
        {
            if (File.Exists(file) && Discovery.Read(file) is { } ep && ep.Pid == pid)
                return ep;
            if (PInvoke.WaitForSingleObject(process, 0) == WAIT_EVENT.WAIT_OBJECT_0)
            {
                PInvoke.GetExitCodeProcess(process, out uint code);
                string tail = LogTail(dir);
                throw CliError.Failure("ERR_GAME_EXITED",
                    $"the game (pid {pid}) exited with code 0x{code:X} before it announced its endpoint" +
                    (tail.Length > 0 ? "; the end of its log:\n" + tail : ""));
            }
            if (DateTime.UtcNow >= deadline)
                throw CliError.Failure("ERR_TIMEOUT",
                    $"the game (pid {pid}) did not announce its endpoint within {timeout.TotalSeconds:0} s; it still runs",
                    $"{Dispatcher.ProgramName} ps --all");
            if (cancel.WaitHandle.WaitOne(250))
                cancel.ThrowIfCancellationRequested();
        }
    }

    // The last lines of the newest file under <dir>\logs, or "".
    public static string LogTail(string dir, int lines = 15)
    {
        try
        {
            var logs = Path.Combine(dir, "logs");
            if (!Directory.Exists(logs))
                return "";
            var newest = new DirectoryInfo(logs).EnumerateFiles("*", SearchOption.AllDirectories)
                .OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault();
            if (newest is null)
                return "";
            using var s = new FileStream(newest.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (s.Length > 64 * 1024)
                s.Seek(-64 * 1024, SeekOrigin.End);
            using var r = new StreamReader(s, Encoding.UTF8);
            var all = r.ReadToEnd().Split('\n').Select(l => l.TrimEnd('\r')).ToList();
            while (all.Count > 0 && all[^1].Length == 0)
                all.RemoveAt(all.Count - 1);
            return newest.FullName + ":\n" + string.Join('\n', all.Skip(Math.Max(0, all.Count - lines)));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return "";
        }
    }

    // --- the manager window ---------------------------------------------------------------------

    public static bool ManagerRunning() => Gui.ManagerInstance.IsRunning();

    // Starts BeDD.exe without arguments (the manager window) unless it runs, detached from this
    // console and job, and waits for its mutex.
    public static void EnsureManager(TimeSpan wait, Action<string>? progress = null)
    {
        if (ManagerRunning())
            return;
        string self = Environment.ProcessPath ?? throw CliError.Failure("ERR_MANAGER", "cannot tell where BeDD.exe is");
        progress?.Invoke("starting the manager window");
        var (pid, process) = Start(self, null, Path.GetDirectoryName(self)!, null, [], detached: true);
        using (process)
        {
            var deadline = DateTime.UtcNow + wait;
            while (!ManagerRunning())
            {
                if (PInvoke.WaitForSingleObject(process, 100) == WAIT_EVENT.WAIT_OBJECT_0 && !ManagerRunning())
                    throw CliError.Failure("ERR_MANAGER",
                        $"the manager window (pid {pid}) exited at once (see {Path.Combine(Paths.Logs, "manager.log")})",
                        $"{Dispatcher.ProgramName} launch --no-manager");
                if (DateTime.UtcNow >= deadline)
                    throw CliError.Failure("ERR_MANAGER",
                        $"the manager window (pid {pid}) did not start within {wait.TotalSeconds:0} s (see {Path.Combine(Paths.Logs, "manager.log")})");
            }
        }
    }

    // --- processes ------------------------------------------------------------------------------

    // Starts exe without activating it, in workDir, outside the caller's job when the job allows it.
    // env adds to (or replaces in) this process's environment for the child only. inherit: the
    // handles the child gets (made inheritable for the call; nothing else is inherited). detached:
    // no console, own process group (for the manager window).
    public static unsafe (int Pid, SafeProcessHandle Process) Start(string exe, string? arguments, string workDir,
        IReadOnlyDictionary<string, string>? env, IReadOnlyList<SafeHandle> inherit, bool detached)
    {
        string commandLine = "\"" + exe + "\"" + (string.IsNullOrEmpty(arguments) ? "" : " " + arguments);
        char[] cmd = (commandLine + "\0").ToCharArray();
        char[]? envBlock = env is null ? null : EnvironmentBlock(env);

        var flags = PROCESS_CREATION_FLAGS.CREATE_UNICODE_ENVIRONMENT;
        if (detached)
            flags |= PROCESS_CREATION_FLAGS.DETACHED_PROCESS | PROCESS_CREATION_FLAGS.CREATE_NEW_PROCESS_GROUP;

        var si = new STARTUPINFOEXW();
        si.StartupInfo.cb = (uint)sizeof(STARTUPINFOW);
        si.StartupInfo.dwFlags = STARTUPINFOW_FLAGS.STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = SwShowNoActivate;

        void* attrs = null;
        var handles = new nint[inherit.Count];
        var added = new List<SafeHandle>();
        try
        {
            if (inherit.Count > 0)
            {
                for (int i = 0; i < inherit.Count; i++)
                {
                    bool ok = false;
                    inherit[i].DangerousAddRef(ref ok);
                    added.Add(inherit[i]);
                    handles[i] = inherit[i].DangerousGetHandle();
                    if (!PInvoke.SetHandleInformation(new HANDLE(handles[i]), (uint)HANDLE_FLAGS.HANDLE_FLAG_INHERIT, HANDLE_FLAGS.HANDLE_FLAG_INHERIT))
                        throw new System.ComponentModel.Win32Exception();
                }
                nuint size = 0;
                PInvoke.InitializeProcThreadAttributeList(default, 1, 0, &size);
                attrs = NativeMemory.Alloc(size);
                if (!PInvoke.InitializeProcThreadAttributeList(new LPPROC_THREAD_ATTRIBUTE_LIST(attrs), 1, 0, &size))
                {
                    NativeMemory.Free(attrs);
                    attrs = null;
                    throw new System.ComponentModel.Win32Exception();
                }
                si.lpAttributeList = new LPPROC_THREAD_ATTRIBUTE_LIST(attrs);
                si.StartupInfo.cb = (uint)sizeof(STARTUPINFOEXW);
                flags |= PROCESS_CREATION_FLAGS.EXTENDED_STARTUPINFO_PRESENT | PROCESS_CREATION_FLAGS.CREATE_SUSPENDED;
            }

            fixed (nint* ph = handles)
            fixed (char* pExe = exe)
            fixed (char* pCmd = cmd)
            fixed (char* pDir = workDir)
            fixed (char* pEnv = envBlock)
            {
                if (attrs is not null &&
                    !PInvoke.UpdateProcThreadAttribute(new LPPROC_THREAD_ATTRIBUTE_LIST(attrs), 0, ProcThreadAttributeHandleList,
                        ph, (nuint)(handles.Length * sizeof(nint)), null, null))
                    throw new System.ComponentModel.Win32Exception();

                PROCESS_INFORMATION pi;
                // Out of the caller's job, so the game outlives a test runner that kills its job when
                // it ends; a job that forbids breaking away refuses with access denied, so then start
                // the game inside it.
                bool created = PInvoke.CreateProcess(pExe, new PWSTR(pCmd), null, null, inherit.Count > 0,
                    flags | PROCESS_CREATION_FLAGS.CREATE_BREAKAWAY_FROM_JOB, pEnv, pDir, (STARTUPINFOW*)&si, &pi);
                if (!created && (uint)Marshal.GetLastPInvokeError() == ErrorAccessDenied)
                    created = PInvoke.CreateProcess(pExe, new PWSTR(pCmd), null, null, inherit.Count > 0,
                        flags, pEnv, pDir, (STARTUPINFOW*)&si, &pi);
                if (!created)
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastPInvokeError(), $"cannot start {exe}");
                if (inherit.Count > 0)
                {
                    // The child's copies are inheritable too, so a helper the game starts with
                    // inheritance on would keep the lock after the game is gone. Replace each by a
                    // non-inheritable duplicate inside the child before it runs.
                    foreach (var h in handles)
                    {
                        HANDLE dup;
                        PInvoke.DuplicateHandle(pi.hProcess, new HANDLE(h), pi.hProcess, &dup, 0, false,
                            DUPLICATE_HANDLE_OPTIONS.DUPLICATE_SAME_ACCESS | DUPLICATE_HANDLE_OPTIONS.DUPLICATE_CLOSE_SOURCE);
                    }
                    if (PInvoke.ResumeThread(pi.hThread) == uint.MaxValue)
                    {
                        int error = Marshal.GetLastPInvokeError();
                        PInvoke.TerminateProcess(pi.hProcess, 1);
                        PInvoke.CloseHandle(pi.hThread);
                        PInvoke.CloseHandle(pi.hProcess);
                        throw new System.ComponentModel.Win32Exception(error, $"cannot start {exe}");
                    }
                }
                PInvoke.CloseHandle(pi.hThread);
                return ((int)pi.dwProcessId, new SafeProcessHandle((nint)pi.hProcess.Value, ownsHandle: true));
            }
        }
        finally
        {
            if (attrs is not null)
            {
                PInvoke.DeleteProcThreadAttributeList(new LPPROC_THREAD_ATTRIBUTE_LIST(attrs));
                NativeMemory.Free(attrs);
            }
            for (int i = 0; i < added.Count; i++)
            {
                // Only the child was meant to inherit it.
                PInvoke.SetHandleInformation(new HANDLE(handles[i]), (uint)HANDLE_FLAGS.HANDLE_FLAG_INHERIT, 0);
                added[i].DangerousRelease();
            }
        }
    }

    // This process's environment with env laid over it, as a CREATE_UNICODE_ENVIRONMENT block:
    // "name=value\0" sorted by name (ignoring case, as Windows wants), then a final "\0".
    internal static char[] EnvironmentBlock(IReadOnlyDictionary<string, string> env)
    {
        var all = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
            if (e.Key is string k && k.Length > 0 && e.Value is string v)
                all[k] = v;
        foreach (var (k, v) in env)
            all[k] = v;
        var sb = new StringBuilder();
        foreach (var (k, v) in all)
            sb.Append(k).Append('=').Append(v).Append('\0');
        sb.Append('\0');
        return sb.ToString().ToCharArray();
    }

    // Every Minecraft.Windows process on the PC (any user's games, any folder).
    public static List<GameProcess> RunningGames()
    {
        var list = new List<GameProcess>();
        foreach (var p in Process.GetProcessesByName(GameProcessName))
        {
            using (p)
            {
                int session = -1;
                try
                {
                    session = p.SessionId;
                }
                catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception)
                {
                }
                list.Add(new GameProcess(p.Id, ImagePath(p.Id) ?? "", session));
            }
        }
        return list;
    }

    // The Minecraft.Windows processes whose exe lies under dir.
    public static List<GameProcess> GamesUnder(string dir) =>
        RunningGames().Where(g => g.Exe.Length > 0 && IsUnder(g.Exe, dir)).ToList();

    // The games of the instance folder dir (or, with anyUnder, of every instance folder under dir):
    // the Minecraft.Windows processes whose exe lies there (unless byExe is false) and the live
    // discovery files that name such a folder (this also finds games whose exe path cannot be read
    // by name, e.g. a BDS).
    public static List<GameProcess> GamesOf(string dir, bool anyUnder = false, bool byExe = true)
    {
        var list = byExe ? GamesUnder(dir) : [];
        foreach (var ep in Discovery.List())
        {
            if (ep.InstanceDir.Length == 0 || list.Any(g => g.Pid == ep.Pid))
                continue;
            if (!(anyUnder ? IsUnder(ep.InstanceDir, dir) : SamePath(ep.InstanceDir, dir)))
                continue;
            if (ImagePath(ep.Pid) is { } exe && IsUnder(exe, ep.InstanceDir))
                list.Add(new GameProcess(ep.Pid, exe));
        }
        list.Sort((a, b) => a.Pid.CompareTo(b.Pid));
        return list;
    }

    // The process of a discovery file still is that game (and not an unrelated process that got the
    // pid of a game that died without deleting its file): its exe lies under the file's instanceDir.
    public static bool IsBeDDProcess(GameEndpoint ep)
    {
        if (ep.InstanceDir.Length == 0)
            return false;
        string? exe = ImagePath(ep.Pid);
        return exe is not null && IsUnder(exe, ep.InstanceDir);
    }

    public static string? ImagePath(int pid)
    {
        using var h = PInvoke.OpenProcess_SafeHandle(PROCESS_ACCESS_RIGHTS.PROCESS_QUERY_LIMITED_INFORMATION, false, (uint)pid);
        if (h.IsInvalid)
            return null;
        Span<char> buf = stackalloc char[32768];
        uint len = (uint)buf.Length;
        if (!PInvoke.QueryFullProcessImageName(h, PROCESS_NAME_FORMAT.PROCESS_NAME_WIN32, buf, ref len))
            return null;
        return new string(buf[..(int)len]);
    }

    // Terminates pid and waits up to wait for it to be gone; false when it cannot be opened or
    // outlives the wait. Deletes its stale run\<pid>.json.
    public static bool Kill(int pid, TimeSpan wait, string? runDir = null)
    {
        bool gone;
        using (var h = PInvoke.OpenProcess_SafeHandle(
                   PROCESS_ACCESS_RIGHTS.PROCESS_TERMINATE | PROCESS_ACCESS_RIGHTS.PROCESS_SYNCHRONIZE | PROCESS_ACCESS_RIGHTS.PROCESS_QUERY_LIMITED_INFORMATION,
                   false, (uint)pid))
        {
            if (h.IsInvalid)
                gone = Marshal.GetLastPInvokeError() == ErrorInvalidParameter; // no such process
            else
            {
                PInvoke.TerminateProcess(h, 1);
                gone = PInvoke.WaitForSingleObject(h, (uint)wait.TotalMilliseconds) == WAIT_EVENT.WAIT_OBJECT_0;
            }
        }
        if (gone)
        {
            try
            {
                File.Delete(Path.Combine(runDir ?? Paths.Run, pid.ToString(CultureInfo.InvariantCulture) + ".json"));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
            }
        }
        return gone;
    }

    // --- small helpers --------------------------------------------------------------------------

    // The instance folder name of a game exe under Paths.Instances ("" elsewhere: its folder's name).
    public static string InstanceOf(string exe, string? instancesDir = null)
    {
        if (exe.Length == 0)
            return "";
        string root = instancesDir ?? Paths.Instances;
        if (IsUnder(exe, root))
        {
            string rel = Path.GetRelativePath(Path.GetFullPath(root), Path.GetFullPath(exe));
            int sep = rel.IndexOfAny(['\\', '/']);
            return sep > 0 ? rel[..sep] : rel;
        }
        return Path.GetFileName(Path.GetDirectoryName(exe)) ?? "";
    }

    // path lies inside dir (not dir itself), comparing full paths without case.
    public static bool IsUnder(string path, string dir)
    {
        string p = Normalize(path);
        string d = Normalize(dir);
        return p.Length > d.Length + 1 && p.StartsWith(d + "\\", StringComparison.OrdinalIgnoreCase);
    }

    public static bool SamePath(string a, string b) =>
        string.Equals(Normalize(a), Normalize(b), StringComparison.OrdinalIgnoreCase);

    private static string Normalize(string p) => Path.GetFullPath(p).TrimEnd('\\', '/');

    // "LiftOff" for a checkout of LiftOff, "LiftOff (fix-x)" in its worktree .claude\worktrees\fix-x;
    // the folder's own name outside a git checkout.
    public static string CallerLabel(string cwd)
    {
        string full = Path.GetFullPath(cwd).TrimEnd('\\', '/');
        for (string? d = full; !string.IsNullOrEmpty(d); d = Path.GetDirectoryName(d))
        {
            string git = Path.Combine(d, ".git");
            if (Directory.Exists(git) || File.Exists(git))
            {
                var m = Regex.Match(d, @"[\\/]([^\\/]+)[\\/]\.claude[\\/]worktrees[\\/]([^\\/]+)$");
                if (m.Success)
                    return $"{m.Groups[1].Value} ({m.Groups[2].Value})";
                return Path.GetFileName(d) is { Length: > 0 } leaf ? leaf : d;
            }
        }
        return Path.GetFileName(full) is { Length: > 0 } name ? name : full;
    }

    // "3m", "90s", "2h", "500ms", or plain seconds.
    public static TimeSpan ParseDuration(string s)
    {
        var m = Regex.Match(s.Trim(), @"^(\d+(?:\.\d+)?)\s*(ms|s|m|min|h)?$", RegexOptions.IgnoreCase);
        if (!m.Success)
            throw CliError.Usage($"'{s}' is not a duration (e.g. 90s, 3m, 2h)");
        double seconds = double.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture) * (m.Groups[2].Value.ToLowerInvariant() switch
        {
            "ms" => 0.001,
            "m" or "min" => 60,
            "h" => 3600,
            _ => 1,
        });
        if (seconds > 30 * 86400)
            throw CliError.Usage($"'{s}' is too long (30 days at most)");
        return TimeSpan.FromSeconds(seconds);
    }
}
