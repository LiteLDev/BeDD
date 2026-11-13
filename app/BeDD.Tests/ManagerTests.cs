using System.Drawing;
using System.Text.Json;
using System.Text.Json.Nodes;
using BeDD.Cli;
using BeDD.Core;
using BeDD.Gui;

namespace BeDD.Tests;

public class TileLayoutTests
{
    [Fact]
    public void NoGamesNoTiles() => Assert.Empty(TileLayout.Arrange(new Rectangle(0, 0, 800, 600), 0, 8, 28));

    [Fact]
    public void OneGameFillsTheAreaAt16By9()
    {
        var t = Assert.Single(TileLayout.Arrange(new Rectangle(0, 34, 1000, 600), 1, 8, 28));
        Assert.Equal(28, t.Caption.Height);
        Assert.Equal(t.Bounds.Width, t.Body.Width);
        Assert.InRange(t.Body.Width / (double)t.Body.Height, 16 / 9.0 - 0.02, 16 / 9.0 + 0.02);
        Assert.True(new Rectangle(0, 34, 1000, 600).Contains(t.Bounds));
    }

    [Fact]
    public void TilesDoNotOverlapAndStayInside()
    {
        var area = new Rectangle(0, 34, 1280, 720);
        for (int n = 1; n <= 9; n++)
        {
            var tiles = TileLayout.Arrange(area, n, 8, 28);
            Assert.Equal(n, tiles.Length);
            for (int i = 0; i < n; i++)
            {
                Assert.True(area.Contains(tiles[i].Bounds), $"{n} tiles: tile {i} {tiles[i].Bounds} outside {area}");
                for (int j = i + 1; j < n; j++)
                    Assert.False(tiles[i].Bounds.IntersectsWith(tiles[j].Bounds), $"{n} tiles: {i} and {j} overlap");
            }
        }
    }

    [Fact]
    public void WideAreasPutTilesSideBySide()
    {
        var tiles = TileLayout.Arrange(new Rectangle(0, 0, 2000, 400), 2, 8, 28);
        Assert.Equal(tiles[0].Bounds.Top, tiles[1].Bounds.Top);
    }

    [Fact]
    public void FitKeepsTheSourceShapeCentred()
    {
        var r = TileLayout.Fit(new Rectangle(0, 0, 400, 400), new Size(800, 400));
        Assert.Equal(new Rectangle(0, 100, 400, 200), r);
        var d = TileLayout.Fit(new Rectangle(10, 10, 160, 90), Size.Empty);
        Assert.Equal(new Rectangle(10, 10, 160, 90), d);
    }
}

public class GameTileTests
{
    private static Game NewGame(string label = "e2e", string instance = "base-3") =>
        new(new GameEndpoint { Pid = 42, Url = "ws://127.0.0.1:5000/v1", Token = "ab", Label = label, Instance = instance });

    [Fact]
    public void TitleIsTheLabelThenTheInstance()
    {
        Assert.Equal("e2e", NewGame().Title);
        Assert.Equal("base-3  ·  pid 42", NewGame().Subtitle);
        Assert.Equal("base-3", NewGame(label: "").Title);
        Assert.Equal("pid 42", NewGame(label: "").Subtitle);
        Assert.Equal("Minecraft", NewGame(label: "", instance: "").Title);
    }

    [Fact]
    public void UrlCarriesTheToken() => Assert.Equal("ws://127.0.0.1:5000/v1?token=ab", NewGame().UrlWithToken);

    [Fact]
    public void StatusFollowsTheConnectionAndInput()
    {
        var g = NewGame();
        var t0 = new DateTime(2026, 1, 1, 12, 0, 0);
        g.Started = t0;
        Assert.Equal("connecting", g.Status(t0).Text);
        g.Connected = true;
        Assert.Equal("starting", g.Status(t0).Text);
        g.Apply(JsonNode.Parse("""{"ok":true,"hwnd":4660,"client":{"w":1280,"h":720},"stats":{"synthetic":5},"clients":1}""")!.AsObject(), t0);
        Assert.Equal(4660, g.Hwnd);
        Assert.Equal(new Size(1280, 720), g.ClientSize);
        Assert.Equal(("idle 30 s", GameStatusKind.Idle), g.Status(t0.AddSeconds(30)));
        g.Apply(JsonNode.Parse("""{"ok":true,"stats":{"synthetic":9},"clients":3}""")!.AsObject(), t0.AddSeconds(40));
        Assert.Equal(("input", GameStatusKind.Active), g.Status(t0.AddSeconds(42)));
        Assert.Equal("idle 2 min  ·  2 clients", g.Status(t0.AddSeconds(160)).Text);
        g.Failures = 3;
        Assert.Equal(GameStatusKind.Bad, g.Status(t0).Kind);
        g.Closing = true;
        Assert.Equal("closing", g.Status(t0).Text);
    }

    [Fact]
    public void ARefusedAttachShows()
    {
        var g = NewGame();
        g.Connected = true;
        g.AttachNote = "unknown op 'manager'";
        Assert.Equal(("not attached", GameStatusKind.Bad), g.Status(DateTime.Now));
        g.Attached = true;
        Assert.NotEqual("not attached", g.Status(DateTime.Now).Text);
    }

    [Fact]
    public void AgoReadsLikeTheOldManager()
    {
        Assert.Equal("5 s", Game.Ago(TimeSpan.FromSeconds(5)));
        Assert.Equal("3 min", Game.Ago(TimeSpan.FromMinutes(3.5)));
        Assert.Equal("2 h 5 min", Game.Ago(TimeSpan.FromMinutes(125)));
    }
}

public class GameMonitorTests
{
    private static async Task Until(Func<bool> condition, string what)
    {
        var deadline = DateTime.UtcNow.AddSeconds(15);
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
                Assert.Fail("timed out waiting for " + what);
            await Task.Delay(50, TestContext.Current.CancellationToken);
        }
    }

    [Fact]
    public async Task AttachesToADiscoveredGameAndDropsItWhenTheConnectionCloses()
    {
        string run = Path.Combine(Path.GetTempPath(), "bedd-monitor-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(run);
        int attaches = 0, states = 0;
        await using var server = new FakeGameServer(req => (string?)req["op"] switch
        {
            "manager" when (string?)req["action"] == "attach" => Count(ref attaches),
            "ping" => new JsonObject { ["ok"] = true, ["hwnd"] = 0, ["client"] = new JsonObject { ["w"] = 640, ["h"] = 360 } },
            "state" => Count(ref states, new JsonObject { ["ok"] = true, ["hwnd"] = 0, ["clients"] = 1, ["stats"] = new JsonObject { ["synthetic"] = states } }),
            _ => new JsonObject { ["ok"] = false, ["error"] = "unknown op" },
        });
        try
        {
            using var monitor = new GameMonitor(run, useJobs: false);
            monitor.Start();
            // A server and a garbage file are ignored.
            await File.WriteAllTextAsync(Path.Combine(run, "1.json"), "{not json", TestContext.Current.CancellationToken);
            var serverEp = server.Endpoint() with { Kind = "server" };
            var ep = server.Endpoint();
            await File.WriteAllTextAsync(Path.Combine(run, "server.json"),
                JsonSerializer.Serialize(serverEp, DiscoveryJson.Default.GameEndpoint), TestContext.Current.CancellationToken);
            await File.WriteAllTextAsync(Path.Combine(run, ep.Pid + ".json"),
                JsonSerializer.Serialize(ep, DiscoveryJson.Default.GameEndpoint), TestContext.Current.CancellationToken);

            await Until(() => { lock (monitor.Sync) return monitor.Games.Count == 1 && monitor.Games[0].Attached; }, "the attach");
            await Until(() => Volatile.Read(ref states) >= 2, "two state calls");
            lock (monitor.Sync)
            {
                var g = monitor.Games[0];
                Assert.Equal(new Size(640, 360), g.ClientSize);
                Assert.Equal("test", g.Title);
            }
            Assert.Equal(1, attaches);
            Assert.Equal(1, server.Connections);

            await server.CloseAllAsync();
            await Until(() => { lock (monitor.Sync) return monitor.Games.Count == 0; }, "the game to go");
            // The same discovery file is not attached to again.
            await Task.Delay(1500, TestContext.Current.CancellationToken);
            lock (monitor.Sync)
                Assert.Empty(monitor.Games);
            Assert.Equal(1, server.Connections);
        }
        finally
        {
            Directory.Delete(run, true);
        }
    }

    private static JsonObject Count(ref int n, JsonObject? reply = null)
    {
        Interlocked.Increment(ref n);
        return reply ?? new JsonObject { ["ok"] = true };
    }
}

// Names of their own: these tests never touch the real manager of this session.
public sealed class ManagerCommandTests : IDisposable
{
    public ManagerCommandTests() => ManagerInstance.SuffixOverride = ".test-" + Guid.NewGuid().ToString("N");

    public void Dispose() => ManagerInstance.SuffixOverride = null;

    [Fact]
    public void NamesHaveTheSessionPrefix()
    {
        Assert.StartsWith(@"Local\BeDD.v2.Manager.test-", ManagerInstance.MutexName);
        Assert.StartsWith(@"Local\BeDD.v2.Manager.Stop", ManagerInstance.StopEventName);
        Assert.StartsWith("BeDD.v2.Manager", ManagerInstance.WindowClass);
    }

    [Fact]
    public void StatusSaysWhetherItRuns()
    {
        var o = new StringWriter();
        var e = new StringWriter();
        int code = Dispatcher.Run(["manager", "status", "--json"], o, e);
        Assert.Equal(0, code);
        var doc = JsonNode.Parse(o.ToString())!;
        Assert.False((bool)doc["running"]!);
        Assert.Equal(ManagerInstance.MutexName, (string?)doc["mutex"]);
    }

    [Fact]
    public void IsRunningFollowsTheMutex()
    {
        // A held mutex of the same name is what a running manager looks like.
        Assert.False(ManagerInstance.IsRunning());
        using var held = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var t = new Thread(() =>
        {
            using var m = new Mutex(true, ManagerInstance.MutexName);
            held.Set();
            release.Wait();
            m.ReleaseMutex();
        });
        t.Start();
        held.Wait(TestContext.Current.CancellationToken);
        Assert.True(ManagerInstance.IsRunning());
        release.Set();
        t.Join();
        Assert.False(ManagerInstance.IsRunning());
        Assert.True(ManagerInstance.WaitForExit(TimeSpan.Zero));
    }

    [Fact]
    public void StopWithoutAManagerIsFine()
    {
        var o = new StringWriter();
        int code = Dispatcher.Run(["manager", "stop", "--yes", "--json"], o, new StringWriter());
        Assert.Equal(0, code);
        Assert.False((bool)JsonNode.Parse(o.ToString())!["stopped"]!);
    }
}
