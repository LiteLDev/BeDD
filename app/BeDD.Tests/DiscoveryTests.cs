using BeDD.Core;

namespace BeDD.Tests;

public sealed class DiscoveryTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "bedd-tests-" + Guid.NewGuid().ToString("N"));

    public DiscoveryTests() => Directory.CreateDirectory(_dir);

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    private void WriteEndpoint(int pid, string startedAt)
    {
        File.WriteAllText(Path.Combine(_dir, $"{pid}.json"), $$"""
            { "protocol": 2, "kind": "client", "pid": {{pid}}, "url": "ws://127.0.0.1:5000/v1",
              "token": "{{new string('a', 64)}}", "instance": "1.26.60.05-2", "instanceDir": "C:\\x",
              "label": "test", "modVersion": "2.0.0", "startedAt": "{{startedAt}}" }
            """);
    }

    [Fact]
    public void ListsLiveEndpointsAndPrunesDeadOnes()
    {
        int self = Environment.ProcessId;
        const int dead = 0x7FFFFFF0; // never a real process id
        WriteEndpoint(self, "2026-11-13T10:00:00Z");
        WriteEndpoint(dead, "2026-11-13T09:00:00Z");
        File.WriteAllText(Path.Combine(_dir, "garbage.json"), "{ not json");

        var live = Discovery.List(prune: true, runDir: _dir);

        var ep = Assert.Single(live);
        Assert.Equal(self, ep.Pid);
        Assert.Equal("ws://127.0.0.1:5000/v1", ep.Url);
        Assert.Equal(64, ep.Token.Length);
        Assert.Equal("1.26.60.05-2", ep.Instance);
        Assert.False(File.Exists(Path.Combine(_dir, $"{dead}.json")));
        Assert.True(File.Exists(Path.Combine(_dir, "garbage.json"))); // not ours to delete
    }

    [Fact]
    public void MissingRunDirIsEmpty() =>
        Assert.Empty(Discovery.List(runDir: Path.Combine(_dir, "nope")));
}
