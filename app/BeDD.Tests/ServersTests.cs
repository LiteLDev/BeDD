using System.Text.Json.Nodes;
using BeDD.Cli;
using BeDD.Core;

namespace BeDD.Tests;

public sealed class ServersTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "bedd-tests-" + Guid.NewGuid().ToString("N"));

    public ServersTests() => Directory.CreateDirectory(_dir);

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    private string Write(string relative, string text)
    {
        var p = Path.Combine(_dir, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(p)!);
        File.WriteAllText(p, text);
        return p;
    }

    [Fact]
    public void ReadsPropertiesWithoutComments()
    {
        var f = Write("server.properties", "# comment\nserver-name=Dedicated Server\n  server-port = 19132 \nbad line\nempty=\n");
        var p = Servers.ReadProperties(f);
        Assert.Equal("Dedicated Server", p["server-name"]);
        Assert.Equal("19132", p["server-port"]);
        Assert.Equal("", p["empty"]);
        Assert.Equal(3, p.Count);
    }

    [Fact]
    public void SetPropertiesKeepsCommentsAndAppendsNewKeys()
    {
        var f = Write("server.properties", "# head\nserver-port=19132\n# about level\nlevel-name=x\n");
        bool changed = Servers.SetProperties(f, [new("server-port", "19140"), new("gamemode", "creative"), new("level-name", "x")]);
        Assert.True(changed);
        Assert.Equal(["# head", "server-port=19140", "# about level", "level-name=x", "gamemode=creative"], File.ReadAllLines(f));
        Assert.False(Servers.SetProperties(f, [new("server-port", "19140")]));
    }

    [Fact]
    public void SetPropertiesLastValueWins()
    {
        var f = Path.Combine(_dir, "new", "server.properties");
        Servers.SetProperties(f, [new("a", "1"), new("a", "2")]);
        Assert.Equal(["a=2"], File.ReadAllLines(f));
    }

    [Fact]
    public void FreePortSkipsOtherServersAndListeners()
    {
        Write(@"s1\server.properties", "server-port=19140\nserver-portv6=19141\n");
        Write(@"s2\server.properties", "server-port=19142\nserver-portv6=19143\n");
        Assert.Equal(19146, Servers.FindFreePort(_dir, null, new HashSet<int> { 19145 }));
        // A server's own ports are free for it.
        Assert.Equal(19140, Servers.FindFreePort(_dir, "s1", new HashSet<int>()));
    }

    [Fact]
    public void UdpPortsOnlyForLoopbackNetherNet()
    {
        Assert.Equal("127.0.0.1:19140:19140", Servers.UdpPorts("nethernet", "127.0.0.1", 19140));
        Assert.Equal("", Servers.UdpPorts("raknet", "127.0.0.1", 19140));
        Assert.Equal("", Servers.UdpPorts("nethernet", "", 19140));
    }

    [Theory]
    [InlineData("s1", true)]
    [InlineData("e2e-49", true)]
    [InlineData("a.b_c", true)]
    [InlineData("..", false)]
    [InlineData("a b", false)]
    [InlineData(@"a\b", false)]
    [InlineData("", false)]
    public void ValidatesNames(string name, bool ok) => Assert.Equal(ok, Servers.IsValidName(name));

    private string MakeModule(string folder, string name = "BeDD-server", string platform = "server", string dll = "x")
    {
        Write($@"{folder}\manifest.json", $$"""{ "name": "{{name}}", "entry": "{{name}}.dll", "platform": "{{platform}}" }""");
        Write($@"{folder}\{name}.dll", dll);
        return Path.Combine(_dir, folder);
    }

    [Fact]
    public void ModSourceOrder()
    {
        var exeDir = Path.Combine(_dir, "release");
        MakeModule(@"release\mod\BeDD-server");
        var tree = Path.Combine(_dir, "tree");
        MakeModule(@"tree\bin\BeDD-server");
        var deep = Path.Combine(tree, "a", "b");
        Directory.CreateDirectory(deep);

        Assert.Equal(Path.GetFullPath("x"), Servers.FindModSource("x", "y", exeDir, deep));
        Assert.Equal(Path.GetFullPath("y"), Servers.FindModSource(null, "y", exeDir, deep));
        Assert.Equal(Path.Combine(exeDir, "mod", "BeDD-server"), Servers.FindModSource(null, null, exeDir, deep));
        Assert.Equal(Path.Combine(tree, "bin", "BeDD-server"), Servers.FindModSource(null, " ", Path.Combine(_dir, "none"), deep));
        Assert.Null(Servers.FindModSource(null, null, Path.Combine(_dir, "none"), Path.Combine(_dir, "none")));
    }

    [Fact]
    public void CopiesModuleAndAdoptsTheConfigOfAMisnamedOne()
    {
        var src = MakeModule("src", dll: "v1");
        var mods = Path.Combine(_dir, @"srv\mods");
        // An older install under another folder name, with its config.
        Write(@"srv\mods\BeDD\manifest.json", """{ "name": "BeDD-server", "entry": "BeDD-server.dll", "platform": "server" }""");
        Write(@"srv\mods\BeDD\config\config.json", """{ "loopbackOnly": true, "pipeName": "BeDD-Server-s1", "pipeInstances": 4, "trace": true }""");

        Assert.True(Servers.CopyModule(src, mods, () => false));
        Assert.False(Directory.Exists(Path.Combine(mods, "BeDD")));
        Assert.Equal("v1", File.ReadAllText(Path.Combine(mods, "BeDD-server", "BeDD-server.dll")));
        Assert.True(File.Exists(Path.Combine(mods, "BeDD-server", "config", "config.json")));
        Assert.False(Servers.CopyModule(src, mods, () => throw new InvalidOperationException("not asked when unchanged")));

        Assert.True(Servers.CleanModuleConfig(Path.Combine(mods, "BeDD-server")));
        var cfg = JsonNode.Parse(File.ReadAllText(Path.Combine(mods, "BeDD-server", "config", "config.json")))!.AsObject();
        Assert.False(cfg.ContainsKey("pipeName"));
        Assert.False(cfg.ContainsKey("pipeInstances"));
        Assert.True((bool)cfg["trace"]!);
        Assert.False(Servers.CleanModuleConfig(Path.Combine(mods, "BeDD-server")));

        File.WriteAllText(Path.Combine(src, "BeDD-server.dll"), "v2");
        Assert.Throws<IOException>(() => Servers.CopyModule(src, mods, () => true));
    }

    [Fact]
    public void RefusesTheClientModule()
    {
        var src = MakeModule("client", name: "BeDD", platform: "client");
        Assert.Throws<InvalidDataException>(() => Servers.CopyModule(src, Path.Combine(_dir, "mods"), () => false));
    }

    [Fact]
    public void GetReportsProblemsAndModule()
    {
        var s = Servers.Get("s1", _dir);
        Assert.Equal(["missing", "leviLamina", "bedd"], s.Problems);

        Write(@"s1\bedrock_server.exe.bak", "");
        Write(@"s1\bedrock_server_mod.exe", "");
        Write(@"s1\mods\LeviLamina\LeviLamina.dll", "");
        Write(@"s1\server.properties", "server-port=19150\nserver-portv6=19151\n");
        MakeModule(@"s1\mods\BeDD-server");
        s = Servers.Get("s1", _dir);
        Assert.Empty(s.Problems);
        Assert.True(s.Installed);
        Assert.Equal(19150, s.Port);
        Assert.Equal(19151, s.PortV6);
        Assert.True(s.ModuleLoads);
        Assert.True(s.LoopbackOnly);
        Assert.Null(Servers.FirewallProblem(s));

        Write(@"s1\mods\BeDD-server\config\config.json", """{ "loopbackOnly": false }""");
        s = Servers.Get("s1", _dir);
        Assert.False(s.LoopbackOnly);
        Assert.Contains("loopbackOnly", Servers.FirewallProblem(s));
        Assert.Equal(["s1"], Servers.List(_dir).Select(x => x.Name));
    }

    [Fact]
    public void StopEventIsPerFolder()
    {
        Assert.Equal(Servers.StopEventName(@"C:\x\s1"), Servers.StopEventName(@"c:\X\S1\"));
        Assert.NotEqual(Servers.StopEventName(@"C:\x\s1"), Servers.StopEventName(@"C:\y\s1"));
        Assert.StartsWith(@"Global\BeDD.Server.", Servers.StopEventName(@"C:\x\s1"));
    }

    // The mod's discovery file is written by another unit; a fake one stands in for it here.
    [Fact]
    public void WaitsForTheServerEndpoint()
    {
        var run = Path.Combine(_dir, "run");
        Directory.CreateDirectory(run);
        int self = Environment.ProcessId;
        void Fake(string kind) => File.WriteAllText(Path.Combine(run, $"{self}.json"), $$"""
            { "protocol": 2, "kind": "{{kind}}", "pid": {{self}}, "url": "ws://127.0.0.1:5001/v1",
              "token": "{{new string('b', 64)}}", "instance": "s1", "instanceDir": "C:\\x", "startedAt": "2026-11-13T10:00:00Z" }
            """);

        Fake("client"); // a game with the same pid is not the server
        Assert.Null(Servers.WaitForEndpoint(self, DateTime.UtcNow.AddMilliseconds(300), run));

        var writer = new Thread(() =>
        {
            Thread.Sleep(400);
            Fake("server");
        });
        writer.Start();
        var ep = Servers.WaitForEndpoint(self, DateTime.UtcNow.AddSeconds(10), run);
        writer.Join();
        Assert.NotNull(ep);
        Assert.Equal("ws://127.0.0.1:5001/v1", ep.Url);
        Assert.Equal(new string('b', 64), ep.Token);

        // A dead server is not waited for.
        var start = DateTime.UtcNow;
        Assert.Null(Servers.WaitForEndpoint(0x7FFFFFF0, DateTime.UtcNow.AddSeconds(10), run, _ => false));
        Assert.True(DateTime.UtcNow - start < TimeSpan.FromSeconds(5));
    }

    [Fact]
    public void ParsesSets()
    {
        var sets = ServerCommands.ParseSets(["level-name=my world", "gamemode = survival", "level-seed="]);
        Assert.Equal([new("level-name", "my world"), new("gamemode", "survival"), new("level-seed", "")], sets);
        Assert.Throws<CliError>(() => ServerCommands.ParseSets(["noequals"]));
        Assert.Throws<CliError>(() => ServerCommands.ParseSets(["=x"]));
    }

    [Fact]
    public void LipHelpers()
    {
        Assert.True(Lip.IsProgressLine("  12.3 / 45.6 MiB [=====>   ]"));
        Assert.True(Lip.IsProgressLine("Downloading https://github.com/LiteLDev/LeviLamina/releases/download/v1/x.zip: 78%"));
        Assert.False(Lip.IsProgressLine("installed github.com/LiteLDev/LeviLamina"));
        Assert.Equal("10.0.15", Lip.LatestVersion("10.0.15\r\n"));
        Assert.Equal("10.0.15", Lip.LatestVersion("abcdef0123\n10.0.15\n"));
        Assert.Null(Lip.LatestVersion("<html>"));

        Assert.False(Lip.HasRuntime(Path.Combine(_dir, "dotnet")));
        Directory.CreateDirectory(Path.Combine(_dir, @"dotnet\shared\Microsoft.NETCore.App\8.0.1"));
        Assert.False(Lip.HasRuntime(Path.Combine(_dir, "dotnet")));
        Directory.CreateDirectory(Path.Combine(_dir, @"dotnet\shared\Microsoft.NETCore.App\10.0.2"));
        Assert.True(Lip.HasRuntime(Path.Combine(_dir, "dotnet")));

        Write(@"path\lip.exe", "");
        Assert.Equal(Path.Combine(_dir, "path", "lip.exe"), Lip.FindOnPath("lip.exe", $"C:\\nope;\"{Path.Combine(_dir, "path")}\""));
    }

    [Fact]
    public void CommandsAreRegistered()
    {
        var o = new StringWriter();
        Assert.Equal(0, Dispatcher.Run(["help", "server"], o, new StringWriter()));
        foreach (var verb in (string[])["init", "start", "stop", "list", "remove", "path"])
            Assert.Contains(verb, o.ToString());
        var e = new StringWriter();
        Assert.Equal((int)ExitCode.Usage, Dispatcher.Run(["server", "stop"], new StringWriter(), e));
        Assert.Equal((int)ExitCode.Usage, Dispatcher.Run(["server", "init", "bad name"], new StringWriter(), new StringWriter()));
    }
}
