using System.Text.Json.Nodes;
using BeDD.Cli;
using BeDD.Core;

namespace BeDD.Tests;

public class ForegroundWatchFormatTests
{
    private static readonly DateTime At = new(2026, 11, 13, 9, 5, 7, 42);

    [Fact]
    public void LineHasTimeKindAndText()
    {
        Assert.Equal("09:05:07.042 clip: full", ForegroundWatch.Line(At, "clip", "full"));
    }

    [Fact]
    public void MinecraftLineIsFlagged()
    {
        var line = ForegroundWatch.Line(At, "foreground", "Minecraft.Windows (pid 7) 'Minecraft'", minecraft: true);
        Assert.EndsWith(ForegroundWatch.MinecraftFlag, line);
        Assert.StartsWith("09:05:07.042 foreground: Minecraft.Windows (pid 7)", line);
    }

    [Fact]
    public void DescribeWindow()
    {
        Assert.Equal("<none>", ForegroundWatch.Describe(null));
        Assert.Equal("notepad (pid 12) 'x - Notepad'", ForegroundWatch.Describe(new WatchedWindow(1, 12, "notepad", "x - Notepad")));
    }

    [Theory]
    [InlineData("Minecraft.Windows", "Minecraft", true)]
    [InlineData("Minecraft.Windows", "", true)]
    [InlineData("?", "Minecraft", true)]
    [InlineData("msedge", "Minecraft Wiki - Edge", true)]
    [InlineData("msedge", "How to play Minecraft", false)]
    [InlineData("WindowsTerminal", "pwsh", false)]
    public void IsMinecraft(string process, string title, bool expected)
    {
        Assert.Equal(expected, ForegroundWatch.IsMinecraft(new WatchedWindow(1, 1, process, title)));
    }

    [Fact]
    public void ClipCoveringTheVirtualDesktopIsFull()
    {
        Assert.Equal("full", ForegroundWatch.ClipText(-1920, 0, 2560, 1440, -1920, 0, 4480, 1440));
        Assert.Equal("100,100-900,700", ForegroundWatch.ClipText(100, 100, 900, 700, -1920, 0, 4480, 1440));
    }

    [Fact]
    public void HeaderNamesForegroundAndClip()
    {
        var h = ForegroundWatch.Header(At, "pwsh (pid 1) 'x'", "full");
        Assert.StartsWith("# 2026-11-13T09:05:07.0420000", h);
        Assert.EndsWith("foreground: pwsh (pid 1) 'x'; clip: full", h);
    }

    [Fact]
    public void WatchWritesAHeaderAndStops()
    {
        var lines = new List<string>();
        using (var w = new ForegroundWatch(l => { lock (lines) lines.Add(l); }))
            w.Start();
        lock (lines)
            Assert.StartsWith("# ", lines[0]);
    }
}

public class DevCommandTests
{
    private static (int Code, string Out, string Err) Run(params string[] args)
    {
        var o = new StringWriter();
        var e = new StringWriter();
        int code = Dispatcher.Run(args, o, e);
        return (code, o.ToString(), e.ToString());
    }

    [Fact]
    public void HelpListsTheFlags()
    {
        var (code, stdout, _) = Run("dev", "--help");
        Assert.Equal(0, code);
        foreach (var f in new[] { "--slot", "--from", "--label", "--watch-log", "--keep", "--watch-only" })
            Assert.Contains(f, stdout);
        Assert.Contains("(default 9)", stdout);
    }

    [Fact]
    public void MissingModIsNotFound()
    {
        var empty = Path.Combine(Path.GetTempPath(), "bedd-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(empty);
        try
        {
            // Until every step command is registered, dev stops at the first missing one instead.
            bool complete = DevCommands.Steps.All(s => Registry.Find(s.Split(' ')).Command?.Path == s);
            var (code, stdout, _) = Run("dev", "--from", empty, "--json");
            Assert.Equal((int)(complete ? ExitCode.NotFound : ExitCode.Failure), code);
            Assert.Equal(complete ? "ERR_MOD_NOT_BUILT" : "ERR_COMMAND_MISSING", (string?)JsonNode.Parse(stdout)!["error"]!["code"]);
        }
        finally
        {
            Directory.Delete(empty, true);
        }
    }

    [Fact]
    public void MissingStepIsAClearFailure()
    {
        var e = Assert.Throws<CliError>(() => DevCommands.Require("frob nicate"));
        Assert.Equal("ERR_COMMAND_MISSING", e.Code);
        Assert.Contains("'frob nicate'", e.Message);
    }

    [Fact]
    public void RequireFindsRegisteredCommands()
    {
        Assert.Equal("dev", DevCommands.Require("dev").Path);
    }

    [Fact]
    public void FieldsReadObjectsAndFirstArrayItems()
    {
        var obj = JsonNode.Parse("""{"name":"base-9","dir":"C:\\i\\base-9","pid":42}""");
        Assert.Equal("base-9", DevCommands.StringField(obj, "name"));
        Assert.Equal(42, DevCommands.IntField(obj, "pid"));
        var arr = JsonNode.Parse("""[{"pid":7}]""");
        Assert.Equal(7, DevCommands.IntField(arr, "pid"));
        Assert.Null(DevCommands.StringField(obj, "missing"));
        Assert.Null(DevCommands.IntField(null, "pid"));
    }

    [Fact]
    public void CheckoutIsFoundAboveTheCurrentDirectory()
    {
        var root = Path.Combine(Path.GetTempPath(), "bedd-tests-" + Guid.NewGuid().ToString("N"));
        var deep = Path.Combine(root, "app", "BeDD");
        Directory.CreateDirectory(deep);
        File.WriteAllText(Path.Combine(root, "xmake.lua"), "");
        var elsewhere = Path.Combine(Path.GetTempPath(), "bedd-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(elsewhere);
        try
        {
            Assert.Equal(root, DevCommands.FindCheckout(null, deep, elsewhere));
            Assert.Equal(root, DevCommands.FindCheckout(Path.Combine(root, "bin", "BeDD"), elsewhere, elsewhere));
            Assert.Equal(elsewhere, DevCommands.FindCheckout(null, elsewhere, elsewhere));
        }
        finally
        {
            Directory.Delete(root, true);
            Directory.Delete(elsewhere, true);
        }
    }
}
