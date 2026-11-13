// Lock and process tests block on purpose: they model one CLI process waiting for another.
#pragma warning disable xUnit1031, xUnit1051

using System.Text.Json.Nodes;
using BeDD.Cli;
using BeDD.Core;
using Microsoft.Win32.SafeHandles;

namespace BeDD.Tests;

public sealed class SignalingLockTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "bedd-tests-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try
        {
            Directory.Delete(_dir, recursive: true);
        }
        catch (IOException)
        {
        }
    }

    [Fact]
    public void SharedHoldersShareAndKeepExclusiveOut()
    {
        using var a = SignalingLock.TryAcquire(false, _dir);
        using var b = SignalingLock.TryAcquire(false, _dir);
        Assert.NotNull(a);
        Assert.NotNull(b);
        Assert.False(a.Exclusive);
        Assert.Null(SignalingLock.TryAcquire(true, _dir));
        a.Dispose();
        Assert.Null(SignalingLock.TryAcquire(true, _dir));
        b.Dispose();
        using var x = SignalingLock.TryAcquire(true, _dir);
        Assert.NotNull(x);
        Assert.True(x.Exclusive);
    }

    [Fact]
    public void ExclusiveHolderKeepsEveryoneOut()
    {
        using var x = SignalingLock.TryAcquire(true, _dir);
        Assert.NotNull(x);
        Assert.Null(SignalingLock.TryAcquire(false, _dir));
        Assert.Null(SignalingLock.TryAcquire(true, _dir));
        x.Dispose();
        using var s = SignalingLock.TryAcquire(false, _dir);
        Assert.NotNull(s);
    }

    [Fact]
    public void WaitingExclusiveQueuesNewSharedHolders()
    {
        using var shared = SignalingLock.TryAcquire(false, _dir)!;
        var lines = new List<string>();
        // The exclusive run waits (holding .wait) while the shared holder runs; a new shared holder
        // cannot get in meanwhile.
        var waiter = Task.Run(() => SignalingLock.Acquire(true, TimeSpan.FromSeconds(20), l => { lock (lines) lines.Add(l); }, _dir));
        SpinWait.SpinUntil(() => { lock (lines) return lines.Count > 0; }, TimeSpan.FromSeconds(10));
        Assert.Null(SignalingLock.TryAcquire(false, _dir));
        shared.Dispose();
        using var x = waiter.GetAwaiter().GetResult();
        Assert.True(x.Exclusive);
        Assert.Contains(lines, l => l.Contains("exclusive"));
    }

    [Fact]
    public void AcquireTimesOut()
    {
        using var x = SignalingLock.TryAcquire(true, _dir)!;
        var e = Assert.Throws<TimeoutException>(() => SignalingLock.Acquire(false, TimeSpan.FromMilliseconds(600), dir: _dir));
        Assert.Contains("shared signaling lock", e.Message);
    }

    [Fact]
    public void ExclusiveWaitsForGamesOutsideTheLock()
    {
        int calls = 0;
        using var x = SignalingLock.Acquire(true, TimeSpan.FromSeconds(20), dir: _dir,
            othersRunning: () => ++calls < 2 ? ["1 other"] : []);
        Assert.Equal(2, calls);
        Assert.Throws<TimeoutException>(() =>
            SignalingLock.Acquire(true, TimeSpan.FromMilliseconds(300), dir: Path.Combine(_dir, "b"), othersRunning: () => ["1 other"]));
        // The timed-out attempt released what it held.
        using var again = SignalingLock.TryAcquire(true, Path.Combine(_dir, "b"));
        Assert.NotNull(again);
    }

    [Fact]
    public void AChildProcessKeepsTheInheritedLock()
    {
        var held = SignalingLock.TryAcquire(false, _dir)!;
        string ping = Path.Combine(Environment.SystemDirectory, "PING.EXE");
        var (pid, process) = Launcher.Start(ping, "-n 30 127.0.0.1", _dir,
            new Dictionary<string, string> { ["BEDD_LABEL"] = "test" }, [held.Handle], detached: true);
        using (process)
        {
            try
            {
                held.Dispose();
                // The child's copy of the handle still holds the lock shared.
                Assert.Null(SignalingLock.TryAcquire(true, _dir));
                using (var other = SignalingLock.TryAcquire(false, _dir))
                    Assert.NotNull(other);
            }
            finally
            {
                Assert.True(Launcher.Kill(pid, TimeSpan.FromSeconds(10), runDir: _dir));
            }
        }
        Assert.True(SpinWait.SpinUntil(() => { using var l = SignalingLock.TryAcquire(true, _dir); return l is not null; },
            TimeSpan.FromSeconds(10)));
    }
}

public sealed class LauncherTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "bedd-tests-" + Guid.NewGuid().ToString("N"));

    public LauncherTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try
        {
            Directory.Delete(_dir, recursive: true);
        }
        catch (IOException)
        {
        }
    }

    [Theory]
    [InlineData("90", 90)]
    [InlineData("90s", 90)]
    [InlineData("3m", 180)]
    [InlineData("2h", 7200)]
    [InlineData("1.5m", 90)]
    [InlineData("500ms", 0.5)]
    public void ParsesDurations(string text, double seconds) =>
        Assert.Equal(seconds, Launcher.ParseDuration(text).TotalSeconds, 3);

    [Theory]
    [InlineData("soon")]
    [InlineData("-3m")]
    [InlineData("99999999999h")]
    public void RejectsBadDurations(string text) =>
        Assert.Equal(ExitCode.Usage, Assert.Throws<CliError>(() => Launcher.ParseDuration(text)).Exit);

    [Fact]
    public void PathHelpers()
    {
        Assert.True(Launcher.IsUnder(@"C:\a\b\Minecraft.Windows.exe", @"C:\A\b"));
        Assert.True(Launcher.IsUnder(@"C:\a\b\c\d.exe", @"C:\a\b\"));
        Assert.False(Launcher.IsUnder(@"C:\a\bc\d.exe", @"C:\a\b"));
        Assert.False(Launcher.IsUnder(@"C:\a\b", @"C:\a\b"));
        Assert.True(Launcher.SamePath(@"C:\a\b\", @"c:\A\B"));
        Assert.Equal("1.26.60.05-2", Launcher.InstanceOf(@"C:\i\1.26.60.05-2\Minecraft.Windows.exe", @"C:\i"));
        Assert.Equal("x", Launcher.InstanceOf(@"D:\games\x\Minecraft.Windows.exe", @"C:\i"));
    }

    [Fact]
    public void CallerLabelNamesRepositoryAndWorktree()
    {
        string repo = Path.Combine(_dir, "LiftOff");
        string wt = Path.Combine(repo, ".claude", "worktrees", "fix-x");
        Directory.CreateDirectory(Path.Combine(repo, ".git"));
        Directory.CreateDirectory(Path.Combine(wt, "src"));
        File.WriteAllText(Path.Combine(wt, ".git"), "gitdir: elsewhere");
        Assert.Equal("LiftOff", Launcher.CallerLabel(Path.Combine(repo)));
        Assert.Equal("LiftOff (fix-x)", Launcher.CallerLabel(Path.Combine(wt, "src")));
    }

    [Fact]
    public void EnvironmentBlockIsSortedAndOverrides()
    {
        var block = new string(Launcher.EnvironmentBlock(new Dictionary<string, string> { ["BEDD_LABEL"] = "x y", ["Path"] = "p" }));
        Assert.EndsWith("\0\0", block);
        var vars = block.TrimEnd('\0').Split('\0');
        Assert.Contains("BEDD_LABEL=x y", vars);
        Assert.EndsWith("=p", Assert.Single(vars, v => v.StartsWith("PATH=", StringComparison.OrdinalIgnoreCase)));
        var names = vars.Select(v => v[..v.IndexOf('=', 1)]).ToList();
        Assert.Equal(names.Order(StringComparer.OrdinalIgnoreCase), names);
    }

    private (int Pid, SafeProcessHandle Process) StartSleeper(int seconds) =>
        Launcher.Start(Path.Combine(Environment.SystemDirectory, "PING.EXE"), $"-n {seconds} 127.0.0.1", _dir, null, [], detached: true);

    [Fact]
    public void WaitForEndpointReadsTheDiscoveryFile()
    {
        var (pid, process) = StartSleeper(30);
        using (process)
        {
            try
            {
                string run = Path.Combine(_dir, "run");
                Directory.CreateDirectory(run);
                var writer = Task.Run(async () =>
                {
                    await Task.Delay(500);
                    File.WriteAllText(Path.Combine(run, $"{pid}.json"),
                        $$"""{ "protocol": 2, "kind": "client", "pid": {{pid}}, "url": "ws://127.0.0.1:5000/v1", "token": "{{new string('b', 64)}}", "instance": "t", "instanceDir": "C:\\t" }""");
                });
                var ep = Launcher.WaitForEndpoint(pid, process, _dir, TimeSpan.FromSeconds(20), runDir: run);
                writer.Wait();
                Assert.Equal("ws://127.0.0.1:5000/v1", ep.Url);
                Assert.Equal(pid, ep.Pid);
            }
            finally
            {
                Launcher.Kill(pid, TimeSpan.FromSeconds(10), runDir: _dir);
            }
        }
    }

    [Fact]
    public void WaitForEndpointReportsTheLogWhenTheGameDies()
    {
        Directory.CreateDirectory(Path.Combine(_dir, "logs"));
        File.WriteAllText(Path.Combine(_dir, "logs", "old.log"), "old\n");
        File.SetLastWriteTimeUtc(Path.Combine(_dir, "logs", "old.log"), DateTime.UtcNow.AddHours(-1));
        File.WriteAllText(Path.Combine(_dir, "logs", "latest.log"), string.Join('\n', Enumerable.Range(1, 40).Select(i => $"line {i}")) + "\n");
        var (pid, process) = StartSleeper(1);
        using (process)
        {
            var e = Assert.Throws<CliError>(() =>
                Launcher.WaitForEndpoint(pid, process, _dir, TimeSpan.FromSeconds(20), runDir: Path.Combine(_dir, "run")));
            Assert.Equal("ERR_GAME_EXITED", e.Code);
            Assert.Contains("line 40", e.Message);
            Assert.DoesNotContain("line 20\n", e.Message);
            Assert.DoesNotContain("old", e.Message);
        }
    }

    [Fact]
    public void WaitForEndpointTimesOut()
    {
        var (pid, process) = StartSleeper(30);
        using (process)
        {
            try
            {
                var e = Assert.Throws<CliError>(() =>
                    Launcher.WaitForEndpoint(pid, process, _dir, TimeSpan.FromMilliseconds(500), runDir: Path.Combine(_dir, "run")));
                Assert.Equal("ERR_TIMEOUT", e.Code);
            }
            finally
            {
                Launcher.Kill(pid, TimeSpan.FromSeconds(10), runDir: _dir);
            }
        }
    }
}

public class LaunchCommandTests
{
    private static (int Code, string Out, string Err) Run(params string[] args)
    {
        var o = new StringWriter();
        var e = new StringWriter();
        int code = Dispatcher.Run(args, o, e);
        return (code, o.ToString(), e.ToString());
    }

    [Fact]
    public void LaunchNeedsTheMod()
    {
        string dir = Path.Combine(Path.GetTempPath(), "bedd-tests-" + Guid.NewGuid().ToString("N"), "inst-1");
        Directory.CreateDirectory(dir);
        try
        {
            var (code, _, err) = Run("play", "--dir", dir, "--no-manager");
            Assert.Equal((int)ExitCode.NotFound, code);
            Assert.Contains("Minecraft.Windows.exe", err);

            File.WriteAllBytes(Path.Combine(dir, "Minecraft.Windows.exe"), []);
            (code, var stdout, _) = Run("launch", "--dir", dir, "--no-manager", "--json");
            Assert.Equal((int)ExitCode.Failure, code);
            var error = JsonNode.Parse(stdout)!["error"]!;
            Assert.Equal("ERR_MOD_MISSING", (string?)error["code"]);
            Assert.Equal("BeDD.exe mod install inst-1", (string?)error["hint"]);
        }
        finally
        {
            Directory.Delete(Path.GetDirectoryName(dir)!, recursive: true);
        }
    }

    [Fact]
    public void LaunchRejectsInstanceAndDir()
    {
        var (code, _, _) = Run("launch", "x", "--dir", Path.GetTempPath());
        Assert.Equal((int)ExitCode.Usage, code);
    }

    [Fact]
    public void StopNeedsExactlyOneTarget()
    {
        Assert.Equal((int)ExitCode.Usage, Run("stop").Code);
        Assert.Equal((int)ExitCode.Usage, Run("stop", "x", "--all").Code);
    }

    [Fact]
    public void StopRefusesAProcessThatIsNotAGame()
    {
        var (code, _, err) = Run("stop", "--pid", Environment.ProcessId.ToString());
        Assert.Equal((int)ExitCode.Conflict, code);
        Assert.Contains("not a BeDD game", err);
        Assert.True(Discovery.IsAlive(Environment.ProcessId));
    }

    [Fact]
    public void PsPrintsAJsonArray()
    {
        var (code, stdout, _) = Run("ps", "--json");
        Assert.Equal(0, code);
        Assert.IsType<JsonArray>(JsonNode.Parse(stdout));
    }
}
