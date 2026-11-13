using System.Diagnostics;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Gui;

// Finds the games and keeps a connection to each: watches the run folder for discovery files, and
// for every game (kind "client") connects over WebSocket, attaches as its manager (the mod then
// keeps the game window hidden, and closes the game when this connection drops), puts it into a
// kill-on-close job and asks for its state every second. A game goes from the list when its
// connection closes or its process exits. Changed fires (on a worker thread) whenever the list or
// a game's state changes.
internal sealed class GameMonitor : IDisposable
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan CallTimeout = TimeSpan.FromSeconds(3);
    private const int MaxConnectAttempts = 5;

    private readonly string _runDir;
    private readonly bool _useJobs;
    private readonly List<Game> _games = [];
    private readonly Dictionary<string, (int Attempts, DateTime Next)> _failed = new(StringComparer.Ordinal);
    private readonly HashSet<string> _dropped = new(StringComparer.Ordinal);
    private readonly List<(int Pid, KillJob Job)> _orphans = [];
    private readonly CancellationTokenSource _stop = new();
    private FileSystemWatcher? _watcher;
    private Timer? _timer;
    private int _scanning;
    private volatile bool _stopped;

    public GameMonitor(string? runDir = null, bool useJobs = true)
    {
        _runDir = runDir ?? Paths.Run;
        _useJobs = useJobs;
    }

    // Guards the games and their fields.
    public object Sync { get; } = new();

    public event Action? Changed;

    // The games, oldest first. Read under Sync.
    public IReadOnlyList<Game> Games => _games;

    public Game[] Snapshot()
    {
        lock (Sync)
            return [.. _games];
    }

    public void Start()
    {
        try
        {
            Directory.CreateDirectory(_runDir);
            _watcher = new FileSystemWatcher(_runDir, "*.json") { NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite };
            _watcher.Created += (_, _) => Scan();
            _watcher.Changed += (_, _) => Scan();
            _watcher.Renamed += (_, _) => Scan();
            _watcher.Error += (_, e) => ManagerLog.Write("watcher: " + e.GetException().Message);
            _watcher.EnableRaisingEvents = true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException)
        {
            ManagerLog.Write("cannot watch " + _runDir + ": " + e.Message + "; polling only");
        }
        // The safety net: missed events, games whose process went, failed connects to retry.
        _timer = new Timer(_ => Scan(), null, TimeSpan.Zero, TimeSpan.FromSeconds(1));
    }

    // One look at the run folder and the processes. Re-entrant calls are skipped.
    public void Scan()
    {
        if (_stopped || Interlocked.Exchange(ref _scanning, 1) != 0)
            return;
        try
        {
            ScanOnce();
        }
        catch (Exception e)
        {
            ManagerLog.Write("scan: " + e);
        }
        finally
        {
            Volatile.Write(ref _scanning, 0);
        }
    }

    private void ScanOnce()
    {
        var endpoints = Discovery.List(prune: true, runDir: _runDir);
        var now = DateTime.UtcNow;
        var added = new List<Game>();
        lock (Sync)
        {
            if (_stopped)
                return;
            foreach (var ep in endpoints)
            {
                if (!string.Equals(ep.Kind, "client", StringComparison.OrdinalIgnoreCase))
                    continue;
                string key = Key(ep);
                if (_dropped.Contains(key) || _games.Any(g => Key(g.Endpoint) == key))
                    continue;
                if (_failed.TryGetValue(key, out var f) && (f.Attempts >= MaxConnectAttempts || now < f.Next))
                    continue;
                // A game restarted under the same pid (a new token): the old entry is stale.
                var stale = _games.FirstOrDefault(g => g.Pid == ep.Pid);
                if (stale is not null)
                    RemoveLocked(stale, "replaced by a new discovery file");
                var game = new Game(ep);
                _games.Add(game);
                added.Add(game);
            }
        }
        foreach (var g in added)
        {
            ManagerLog.Write($"game {g.Pid} ({g.Endpoint.Instance}) found at {g.Endpoint.Url}");
            _ = Task.Run(() => RunGame(g));
        }

        foreach (var g in Snapshot())
            if (!Discovery.IsAlive(g.Pid))
                Remove(g, "exited");
        lock (Sync)
        {
            for (int i = _orphans.Count - 1; i >= 0; i--)
            {
                if (!Discovery.IsAlive(_orphans[i].Pid))
                {
                    _orphans[i].Job.Dispose();
                    _orphans.RemoveAt(i);
                }
            }
        }
        if (added.Count > 0)
            Changed?.Invoke();
    }

    private static string Key(GameEndpoint ep) => ep.Token.Length > 0 ? ep.Token : ep.Pid + "|" + ep.Url;

    private async Task RunGame(Game g)
    {
        GameClient client;
        try
        {
            client = await GameClient.ConnectAsync(g.Endpoint, ConnectTimeout, _stop.Token).ConfigureAwait(false);
        }
        catch (Exception e) when (e is GameClientException or OperationCanceledException)
        {
            lock (Sync)
            {
                string key = Key(g.Endpoint);
                int attempts = _failed.TryGetValue(key, out var f) ? f.Attempts + 1 : 1;
                _failed[key] = (attempts, DateTime.UtcNow + TimeSpan.FromSeconds(1 << Math.Min(attempts, 5)));
                if (attempts >= MaxConnectAttempts)
                    ManagerLog.Write($"game {g.Pid}: giving up after {attempts} attempts");
            }
            Remove(g, e.Message);
            return;
        }
        bool keep;
        lock (Sync)
        {
            keep = _games.Contains(g) && !_stopped;
            if (keep)
            {
                g.Client = client;
                g.Connected = true;
                _failed.Remove(Key(g.Endpoint));
            }
        }
        if (!keep)
        {
            await client.DisposeAsync().ConfigureAwait(false);
            return;
        }
        client.Closed += (_, why) => OnClosed(g, why);
        if (!client.IsOpen)
        {
            OnClosed(g, client.CloseReason);
            return;
        }
        ManagerLog.Write($"game {g.Pid} connected");

        try
        {
            await TryAttach(g, client).ConfigureAwait(false);
            var ping = await client.CallAsync("ping", CallTimeout, _stop.Token).ConfigureAwait(false);
            lock (Sync)
                g.Apply(ping, DateTime.Now);
        }
        catch (GameClientException e)
        {
            ManagerLog.Write($"game {g.Pid}: {e.Message}");
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (_useJobs)
        {
            var job = KillJob.Create(g.Pid, out string note);
            bool gone;
            lock (Sync)
            {
                gone = !_games.Contains(g);
                if (!gone)
                {
                    g.Job = job;
                    g.JobNote = note;
                }
            }
            if (gone)
                Adopt(g.Pid, job);
            ManagerLog.Write(job is not null ? $"game {g.Pid} in a kill-on-close job" : $"game {g.Pid}: {note}");
        }
        Changed?.Invoke();

        while (!_stop.IsCancellationRequested && client.IsOpen)
        {
            (nint, System.Drawing.Size, bool, long, bool) before = default, after = default;
            try
            {
                await Task.Delay(1000, _stop.Token).ConfigureAwait(false);
                bool attached;
                lock (Sync)
                    attached = g.Attached;
                // A game busy loading may miss the first attach; it closes if none arrives in 30 s.
                if (!attached)
                    await TryAttach(g, client).ConfigureAwait(false);
                var state = await client.CallAsync("state", CallTimeout, _stop.Token).ConfigureAwait(false);
                JsonObject? ping = null;
                if (GameClient.IsOk(state) && state["hwnd"] is null)
                    ping = await client.CallAsync("ping", CallTimeout, _stop.Token).ConfigureAwait(false);
                lock (Sync)
                {
                    before = Look(g);
                    if (GameClient.IsOk(state))
                    {
                        g.Failures = 0;
                        g.Apply(state, DateTime.Now);
                        if (ping is not null && GameClient.IsOk(ping))
                            g.Apply(ping, DateTime.Now);
                    }
                    else
                    {
                        g.Failures++;
                    }
                    after = Look(g);
                }
            }
            catch (GameClientException e) when (e.Code == "timeout")
            {
                bool first;
                lock (Sync)
                {
                    before = Look(g);
                    first = ++g.Failures == 3;
                    after = Look(g);
                }
                if (first)
                    ManagerLog.Write($"game {g.Pid} is not answering");
            }
            catch (GameClientException)
            {
                break; // closed: OnClosed removes it
            }
            catch (OperationCanceledException)
            {
                break;
            }
            // The window repaints every second anyway (the status line); a layout is needed only
            // when something it depends on changed.
            if (before != after)
                Changed?.Invoke();
        }
    }

    // What the window's layout and thumbnail depend on.
    private static (nint, System.Drawing.Size, bool, long, bool) Look(Game g) =>
        (g.Hwnd, g.ClientSize, g.Failures >= 3, g.Synthetic, g.Attached);

    private async Task TryAttach(Game g, GameClient client)
    {
        JsonObject attach;
        try
        {
            attach = await client.CallAsync(new JsonObject { ["op"] = "manager", ["action"] = "attach" }, CallTimeout, _stop.Token)
                .ConfigureAwait(false);
        }
        catch (GameClientException e) when (e.Code == "timeout")
        {
            lock (Sync)
                g.AttachNote = "no answer to attach";
            ManagerLog.Write($"game {g.Pid}: attach not answered");
            return;
        }
        bool ok = GameClient.IsOk(attach);
        lock (Sync)
        {
            g.Attached = ok;
            g.AttachNote = ok ? "" : GameClient.Error(attach);
        }
        ManagerLog.Write(ok ? $"game {g.Pid} attached" : $"game {g.Pid}: attach refused: {GameClient.Error(attach)}");
    }

    private void OnClosed(Game g, string why)
    {
        lock (Sync)
        {
            if (!_games.Contains(g))
                return;
            _dropped.Add(Key(g.Endpoint));
        }
        Remove(g, "connection closed: " + why);
    }

    private void Remove(Game g, string why)
    {
        bool removed;
        lock (Sync)
            removed = RemoveLocked(g, why);
        if (removed)
            Changed?.Invoke();
    }

    private bool RemoveLocked(Game g, string why)
    {
        if (!_games.Remove(g))
            return false;
        g.Client?.Dispose();
        g.Client = null;
        if (g.Job is { } job)
        {
            g.Job = null;
            // Still running (its connection dropped, so the mod closes it): the job stays until it
            // exits, so that it still goes with the manager.
            if (Discovery.IsAlive(g.Pid))
                _orphans.Add((g.Pid, job));
            else
                job.Dispose();
        }
        ManagerLog.Write($"game {g.Pid} ({g.Endpoint.Instance}) removed: {why}");
        return true;
    }

    private void Adopt(int pid, KillJob? job)
    {
        if (job is null)
            return;
        lock (Sync)
        {
            if (Discovery.IsAlive(pid))
                _orphans.Add((pid, job));
            else
                job.Dispose();
        }
    }

    // Asks one game to close ({op:"close"}) and kills it if it is still there after 10 s.
    public void CloseGame(Game g)
    {
        GameClient? client;
        lock (Sync)
        {
            if (g.Closing || !_games.Contains(g))
                return;
            g.Closing = true;
            client = g.Client;
        }
        ManagerLog.Write($"closing game {g.Pid} ({g.Endpoint.Instance})");
        Changed?.Invoke();
        _ = Task.Run(async () =>
        {
            using var process = OpenProcess(g.Pid);
            await AskToClose(g, client).ConfigureAwait(false);
            if (process is not null && !await WaitForExit(process, TimeSpan.FromSeconds(10)).ConfigureAwait(false))
            {
                ManagerLog.Write($"game {g.Pid} did not close; killing it");
                Kill(process);
            }
        });
    }

    // Closes every game: {op:"close"} to each, a kill for those still there after 10 s. The monitor
    // stops looking for games first. Blocks until done.
    public void CloseAll()
    {
        Game[] all;
        lock (Sync)
        {
            _stopped = true;
            all = [.. _games];
            foreach (var g in all)
                g.Closing = true;
        }
        _timer?.Dispose();
        if (_watcher is not null)
            _watcher.EnableRaisingEvents = false;
        if (all.Length > 0)
            ManagerLog.Write($"closing {all.Length} game(s)");
        var processes = all.Select(g => (Game: g, Process: OpenProcess(g.Pid))).ToArray();
        try
        {
            var asks = all.Select(g => AskToClose(g, g.Client)).ToArray();
            Task.WaitAll(asks, TimeSpan.FromSeconds(4));
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
            foreach (var (g, p) in processes)
            {
                if (p is null)
                    continue;
                var left = deadline - DateTime.UtcNow;
                if (left > TimeSpan.Zero && p.WaitForExit(left))
                    continue;
                if (!p.HasExited)
                {
                    ManagerLog.Write($"game {g.Pid} did not close in time; killing it");
                    Kill(p);
                }
            }
        }
        finally
        {
            foreach (var (_, p) in processes)
                p?.Dispose();
            _stop.Cancel();
            DisposeAll(); // kills what is left of them
        }
        Changed?.Invoke();
    }

    private static async Task AskToClose(Game g, GameClient? client)
    {
        if (client is null || !client.IsOpen)
            return;
        try
        {
            var r = await client.CallAsync("close", CallTimeout).ConfigureAwait(false);
            if (!GameClient.IsOk(r))
                ManagerLog.Write($"game {g.Pid}: close refused: {GameClient.Error(r)}");
        }
        catch (GameClientException)
        {
            // It may close the connection before it answers.
        }
    }

    private static Process? OpenProcess(int pid)
    {
        try
        {
            return Process.GetProcessById(pid);
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException)
        {
            return null;
        }
    }

    private static async Task<bool> WaitForExit(Process p, TimeSpan timeout)
    {
        using var cts = new CancellationTokenSource(timeout);
        try
        {
            await p.WaitForExitAsync(cts.Token).ConfigureAwait(false);
            return true;
        }
        catch (OperationCanceledException)
        {
            return p.HasExited;
        }
    }

    private static void Kill(Process p)
    {
        try
        {
            p.Kill();
        }
        catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        {
            ManagerLog.Write($"cannot kill {p.Id}: {e.Message}");
        }
    }

    public void Dispose()
    {
        _stopped = true;
        _timer?.Dispose();
        _watcher?.Dispose();
        _stop.Cancel();
        DisposeAll();
    }

    // Drops every connection and closes every job (which kills the games still in them). The lists
    // are emptied first: a dropped connection calls OnClosed, which must find nothing to remove.
    private void DisposeAll()
    {
        Game[] games;
        (int Pid, KillJob Job)[] orphans;
        lock (Sync)
        {
            games = [.. _games];
            orphans = [.. _orphans];
            _games.Clear();
            _orphans.Clear();
        }
        foreach (var g in games)
        {
            g.Client?.Dispose();
            g.Job?.Dispose();
            g.Job = null;
        }
        foreach (var (_, job) in orphans)
            job.Dispose();
    }
}
