using System.Text.Json.Nodes;
using BeDD.Cli;

namespace BeDD.Tests;

public class DispatcherTests
{
    private static (int Code, string Out, string Err) Run(params string[] args)
    {
        var o = new StringWriter();
        var e = new StringWriter();
        int code = Dispatcher.Run(args, o, e);
        return (code, o.ToString(), e.ToString());
    }

    [Fact]
    public void VersionPrintsJson()
    {
        var (code, stdout, _) = Run("version", "--json");
        Assert.Equal(0, code);
        var doc = JsonNode.Parse(stdout)!;
        Assert.Equal(BuildInfo.GameVersion, (string?)doc["gameVersion"]);
        Assert.Equal(BuildInfo.Protocol, (int?)doc["protocol"]);
    }

    [Fact]
    public void VersionQuietPrintsOnlyTheVersion()
    {
        var (code, stdout, _) = Run("version", "-q");
        Assert.Equal(0, code);
        Assert.Equal(BuildInfo.Version, stdout.Trim());
    }

    [Fact]
    public void UnknownCommandIsAUsageError()
    {
        var (code, _, stderr) = Run("frobnicate", "now");
        Assert.Equal((int)ExitCode.Usage, code);
        Assert.Contains("unknown command", stderr);
        Assert.Contains("hint: BeDD.exe help", stderr);
    }

    [Fact]
    public void UsageErrorWithJsonPrintsAnErrorDocument()
    {
        var (code, stdout, _) = Run("version", "--bogus", "--json");
        Assert.Equal((int)ExitCode.Usage, code);
        var err = JsonNode.Parse(stdout)!["error"]!;
        Assert.Equal("ERR_USAGE", (string?)err["code"]);
    }

    [Fact]
    public void HelpListsTopLevelCommands()
    {
        var (code, stdout, _) = Run("help");
        Assert.Equal(0, code);
        Assert.Contains("version", stdout);
    }

    [Fact]
    public void ParseHandlesValueFormsAndNegation()
    {
        var cmd = new Command
        {
            Path = "test parse",
            Summary = "test",
            MaxArgs = 5,
            Flags =
            [
                Flag.String("name", "n", "<name>", "a name"),
                Flag.Int("slot", "", "<n>", "a slot", 1),
                Flag.Bool("force", "f", "force"),
                Flag.Strings("env", "", "<k=v>", "env"),
            ],
            Run = _ => 0,
        };
        var (pos, flags) = Dispatcher.Parse(cmd,
            ["a", "--name=x", "-n", "y", "--slot", "3", "--no-force", "--env", "A=1", "--env", "B=2", "--", "--literal"]);
        Assert.Equal(["a", "--literal"], pos);
        Assert.Equal("y", flags["name"][^1]);
        Assert.Equal("3", flags["slot"][^1]);
        Assert.Equal("false", flags["force"][^1]);
        Assert.Equal(["A=1", "B=2"], flags["env"]);
    }

    [Fact]
    public void ParseRejectsBadInt()
    {
        var cmd = new Command
        {
            Path = "test int",
            Summary = "test",
            Flags = [Flag.Int("slot", "", "<n>", "a slot")],
            Run = _ => 0,
        };
        var e = Assert.Throws<CliError>(() => Dispatcher.Parse(cmd, ["--slot", "two"]));
        Assert.Equal(ExitCode.Usage, e.Exit);
    }
}
