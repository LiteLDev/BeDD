using BeDD.Core;

namespace BeDD.Tests;

public sealed class CloneRecordTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "bedd-rec-" + Guid.NewGuid().ToString("N"));

    public void Dispose() => Directory.Delete(_dir, true);

    [Fact]
    public void AnOlderCloneJsonWithoutListsReadsAsEmptyLists()
    {
        Directory.CreateDirectory(Path.Combine(_dir, ".bedd"));
        File.WriteAllText(Path.Combine(_dir, ".bedd", "clone.json"),
            """{ "base": "b", "baseDir": "C:\\x", "baseGameVersion": "1.26.60.05", "slot": 2, "createdAt": "2026-11-13T15:53:50+08:00" }""");
        var rec = Cloner.ReadRecord(_dir)!;
        Assert.Empty(rec.RootFiles);
        Assert.Empty(rec.Copies);
        Assert.Equal(2, rec.Slot);
    }
}
