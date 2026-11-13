using System.Text.Json.Nodes;
using BeDD.Cli;
using BeDD.Core;

namespace BeDD.Tests;

internal sealed class FakeLauncher(params LeviLauncherInstance[] instances) : ILeviLauncher
{
    public string Exe => @"C:\fake\LeviLauncher.exe";
    public int Calls { get; private set; }

    public IReadOnlyList<LeviLauncherInstance> ListInstances()
    {
        Calls++;
        return instances;
    }
}

public sealed class BaseSelectionTests
{
    private static LeviLauncherInstance I(string name, string game = "1.26.60.05", string? loader = "26.60.5", bool def = false,
        string type = "gdk", string channel = "release") => new()
    {
        Name = name, GameVersion = game, LoaderVersion = loader, IsDefault = def, Type = type, Channel = channel,
        Path = Path.GetTempPath(),
    };

    [Fact]
    public void PrefersTheDefaultInstance()
    {
        var b = BaseSelection.Select([I("a"), I("b", def: true)], null, useEnvironment: false);
        Assert.Equal("b", b.Name);
    }

    [Fact]
    public void FallsBackToTheFirstUsableWhenTheDefaultIsNot()
    {
        var b = BaseSelection.Select([I("old", game: "1.21.0.3", def: true), I("preview", channel: "preview"), I("ok")], null, useEnvironment: false);
        Assert.Equal("ok", b.Name);
    }

    [Fact]
    public void ANamedInstanceIsUsedOrRejected()
    {
        Assert.Equal("B", BaseSelection.Select([I("a"), I("B")], "b", useEnvironment: false).Name);
        var missing = Assert.Throws<CliError>(() => BaseSelection.Select([I("a")], "zzz", useEnvironment: false));
        Assert.Equal("ERR_BASE_NOT_FOUND", missing.Code);
        var noLoader = Assert.Throws<CliError>(() => BaseSelection.Select([I("a", loader: null)], "a", "LL.exe", useEnvironment: false));
        Assert.Equal("ERR_BASE_UNUSABLE", noLoader.Code);
        Assert.Equal("LL.exe loader install \"a\"", noLoader.Hint);
    }

    [Fact]
    public void NoInstanceAtAllHintsAtInstallingOne()
    {
        var e = Assert.Throws<CliError>(() => BaseSelection.Select([I("old", game: "1.21.0.3")], null, "LL.exe", useEnvironment: false));
        Assert.Equal("ERR_NO_BASE", e.Code);
        Assert.Contains("LL.exe account login", e.Hint);
        Assert.Contains($"LL.exe install {BuildInfo.GameVersion} --loader", e.Hint);
    }

    [Fact]
    public void AnInstanceWithoutLeviLaminaHintsAtTheLoader()
    {
        var e = Assert.Throws<CliError>(() => BaseSelection.Select([I("mine", loader: null, def: true)], null, "LL.exe", useEnvironment: false));
        Assert.Equal("LL.exe loader install \"mine\"", e.Hint);
    }

    [Fact]
    public void LoaderLineAndAgeAreChecked()
    {
        Assert.NotNull(BaseSelection.Unusable(I("x", loader: "26.50.1")));
        Assert.NotNull(BaseSelection.Unusable(I("x", type: "uwp")));
        Assert.Null(BaseSelection.Unusable(I("x", game: BuildInfo.GameVersion)));
        Assert.True(BaseSelection.LoaderOlder(I("x", loader: "26.60.0")));
        Assert.False(BaseSelection.LoaderOlder(I("x", loader: "26.60.99")));
    }

    [Fact]
    public void ParsesLeviLaunchersInstanceList()
    {
        var list = LeviLauncherCli.ParseInstances("""
            [ { "name": "我的实例", "gameVersion": "1.26.60.05", "channel": "release", "type": "gdk", "isolated": true,
                "registered": false, "running": false, "pids": [], "loaderVersion": "26.60.5", "console": false,
                "path": "C:\\x\\我的实例", "isDefault": true } ]
            """);
        var i = Assert.Single(list);
        Assert.Equal("我的实例", i.Name);
        Assert.Equal("26.60.5", i.LoaderVersion);
        Assert.True(i.IsDefault);
        Assert.True(i.Isolated);
    }

    [Fact]
    public void ToolsCandidatesPreferTheHighestVersion()
    {
        string tools = Path.Combine(Path.GetTempPath(), "bedd-tools-" + Guid.NewGuid().ToString("N"));
        try
        {
            foreach (var d in new[] { "LeviLauncher", "LeviLauncher-1.1.0", "LeviLauncher-1.0.2" })
            {
                Directory.CreateDirectory(Path.Combine(tools, d));
                File.WriteAllText(Path.Combine(tools, d, "LeviLauncher.exe"), "");
            }
            var c = LeviLauncherCli.ToolsCandidates(tools).Select(p => Path.GetFileName(Path.GetDirectoryName(p))).ToList();
            Assert.Equal(["LeviLauncher-1.1.0", "LeviLauncher-1.0.2", "LeviLauncher"], c);
        }
        finally
        {
            Directory.Delete(tools, true);
        }
    }
}

public sealed class CloneTests : IDisposable
{
    private readonly string _tmp = Path.Combine(Path.GetTempPath(), "bedd-clone-" + Guid.NewGuid().ToString("N"));
    private readonly string _base;
    private readonly string _root;
    private readonly FakeLauncher _launcher;
    private readonly List<string> _warnings = [];

    public CloneTests()
    {
        _base = Path.Combine(_tmp, "LeviLauncher", "Instances", "My instance");
        _root = Path.Combine(_tmp, "instances");
        Directory.CreateDirectory(_base);
        File.WriteAllText(Path.Combine(_base, "Minecraft.Windows.exe"), "game");
        File.WriteAllText(Path.Combine(_base, "Minecraft.Windows.exe.bak"), "backup");
        File.WriteAllText(Path.Combine(_base, "LeviLauncher.dll"), "loader");
        File.WriteAllText(Path.Combine(_base, "PreLoaderConfig.json"), "{}");
        Write("data/assets/a.bin", "asset");
        Write("Installers/x.txt", "inst");
        Write("Minecraft Bedrock/Users/Shared/games/com.mojang/minecraftWorlds/w1/level.dat", "lvl");
        Write(".levilauncher/instance.json", """{ "name": "My instance", "gameVersion": "1.26.60.05", "enableIsolation": false }""");
        Write(".lip/state.json", "{}");
        Write("mods/LeviLamina/LeviLamina.dll", "ll");
        Write("mods/LeviLamina/manifest.json", """{ "name": "LeviLamina", "version": "26.60.98" }""");
        Write("mods/LeviLamina/config/Config.json", """{ "modules": { "crashLogger": { "enabled": true, "builtin": false } } }""");
        Write("mods/SomeoneElse/x.dll", "x");
        Write("logs/old.log", "log");
        _launcher = new FakeLauncher(new LeviLauncherInstance
        {
            Name = "My instance", GameVersion = "1.26.60.05", Channel = "release", Type = "gdk", LoaderVersion = "26.60.98",
            Path = _base, IsDefault = true,
        });
    }

    private void Write(string rel, string text)
    {
        string p = Path.Combine(_base, rel.Replace('/', '\\'));
        Directory.CreateDirectory(Path.GetDirectoryName(p)!);
        File.WriteAllText(p, text);
    }

    public void Dispose()
    {
        // Clones first (junctions), never following them.
        if (Directory.Exists(_root))
            FileLinks.DeleteTree(_root);
        Directory.Delete(_tmp, true);
    }

    private InstanceInfo Clone(int slot = 3, Func<string, string, int>? hardLink = null, bool force = false) =>
        InstanceStore.EnsureClone(new CloneOptions
        {
            Slot = slot, Base = "My instance", Root = _root, Launcher = _launcher, HardLink = hardLink, Force = force,
            Warn = _warnings.Add,
        });

    [Fact]
    public void CloneHasTheExpectedLayout()
    {
        var links = new List<string>();
        var i = Clone(hardLink: (l, e) => { links.Add(Path.GetFileName(l)); return FileLinks.TryHardLink(l, e); });
        string dir = Path.Combine(_root, "1.26.60.05-3");
        Assert.Equal(dir, i.Dir);
        Assert.Equal("1.26.60.05-3", i.Name);
        Assert.Equal(3, i.Slot);
        Assert.Equal("My instance", i.Base);
        Assert.Equal("26.60.98", i.LoaderVersion);
        Assert.Equal(["mod-missing"], i.Problems);

        // Root files linked, json copied, LeviLauncher's exe backups left out.
        Assert.Contains("Minecraft.Windows.exe", links);
        Assert.Contains("LeviLauncher.dll", links);
        Assert.DoesNotContain("PreLoaderConfig.json", links);
        Assert.True(File.Exists(Path.Combine(dir, "PreLoaderConfig.json")));
        Assert.False(File.Exists(Path.Combine(dir, "Minecraft.Windows.exe.bak")));

        // Junctions to the base's assets.
        Assert.Equal(Path.Combine(_base, "data"), FileLinks.LinkTarget(Path.Combine(dir, "data")));
        Assert.Equal(Path.Combine(_base, "Installers"), FileLinks.LinkTarget(Path.Combine(dir, "Installers")));
        Assert.True(File.Exists(Path.Combine(dir, "data", "assets", "a.bin")));

        // Copies, isolation forced on in the clone only.
        Assert.Null(FileLinks.LinkTarget(Path.Combine(dir, "Minecraft Bedrock")));
        Assert.True(File.Exists(Path.Combine(dir, "Minecraft Bedrock", "Users", "Shared", "games", "com.mojang", "minecraftWorlds", "w1", "level.dat")));
        var meta = JsonNode.Parse(File.ReadAllText(Path.Combine(dir, ".levilauncher", "instance.json")))!;
        Assert.True((bool)meta["enableIsolation"]!);
        Assert.Equal("My instance", (string?)meta["name"]);
        Assert.False((bool)JsonNode.Parse(File.ReadAllText(Path.Combine(_base, ".levilauncher", "instance.json")))!["enableIsolation"]!);
        Assert.True(File.Exists(Path.Combine(dir, ".lip", "state.json")));

        // Fresh logs and mods, only the loader's mods copied.
        Assert.Empty(Directory.EnumerateFileSystemEntries(Path.Combine(dir, "logs")));
        Assert.Equal(["LeviLamina"], Directory.EnumerateDirectories(Path.Combine(dir, "mods")).Select(Path.GetFileName));

        var rec = Cloner.ReadRecord(dir)!;
        Assert.Equal("My instance", rec.Base);
        Assert.Equal(_base, rec.BaseDir);
        Assert.Equal("1.26.60.05", rec.BaseGameVersion);
        Assert.Equal(3, rec.Slot);
        Assert.Empty(_warnings);

        // Resolve finds it by name and by slot; a second EnsureClone does not ask LeviLauncher again.
        Assert.Equal(dir, InstanceStore.Resolve(_root, "1.26.60.05-3", 1)!.Dir);
        Assert.Equal(dir, InstanceStore.Resolve(_root, null, 3, "My instance")!.Dir);
        Assert.Null(InstanceStore.Resolve(_root, null, 3, "another"));
        int calls = _launcher.Calls;
        InstanceStore.EnsureClone(new CloneOptions { Slot = 3, Base = "My instance", Root = _root, Launcher = _launcher });
        Assert.Equal(calls, _launcher.Calls);
        Assert.Single(InstanceStore.List(_root));
    }

    [Fact]
    public void CopiesWhenHardLinksAreImpossibleAndWarnsOnce()
    {
        var i = Clone(hardLink: (_, _) => FileLinks.ErrorNotSameDevice);
        Assert.Equal("game", File.ReadAllText(Path.Combine(i.Dir, "Minecraft.Windows.exe")));
        Assert.Equal("loader", File.ReadAllText(Path.Combine(i.Dir, "LeviLauncher.dll")));
        var w = Assert.Single(_warnings);
        Assert.Contains("BEDD_INSTANCES_DIR", w);
        // A copy keeps the time, so the clone does not look stale.
        Assert.DoesNotContain("stale", InstanceStore.Inspect(i.Dir)!.Problems);
    }

    [Fact]
    public void OtherLinkErrorsFail()
    {
        Assert.ThrowsAny<IOException>(() => Clone(hardLink: (_, _) => 5));
        Assert.False(Directory.Exists(Path.Combine(_root, "1.26.60.05-3")));
        Assert.False(Directory.Exists(Path.Combine(_root, "1.26.60.05-3.partial")));
    }

    [Fact]
    public void RefreshRelinksChangedFilesAndRebuildsWhatIsBroken()
    {
        var i = Clone();
        Assert.DoesNotContain("stale", i.Problems);

        // The base updates: a file replaced, a file added, LeviLamina updated, the metadata rewritten.
        string exe = Path.Combine(_base, "Minecraft.Windows.exe");
        File.Delete(exe);
        File.WriteAllText(exe, "game v2");
        File.WriteAllText(Path.Combine(_base, "new.dll"), "new");
        Write("mods/LeviLamina/manifest.json", """{ "name": "LeviLamina", "version": "26.60.99" }""");
        File.SetLastWriteTimeUtc(Path.Combine(_base, ".levilauncher", "instance.json"), DateTime.UtcNow.AddMinutes(1));
        // The clone's data junction is lost.
        FileLinks.RemoveLink(Path.Combine(i.Dir, "data"));

        var before = InstanceStore.Inspect(i.Dir)!;
        Assert.Contains("stale", before.Problems);
        Assert.Contains("junction", before.Problems);

        var (after, changed) = InstanceStore.Refresh(before);
        Assert.Contains("Minecraft.Windows.exe", changed.RootFiles);
        Assert.Contains("new.dll", changed.RootFiles);
        Assert.Equal(["LeviLamina"], changed.LoaderMods);
        Assert.Equal(["data"], changed.Junctions);
        Assert.Single(changed.Metadata);
        Assert.Empty(changed.Removed);
        Assert.Equal(["mod-missing"], after.Problems);
        Assert.Equal("game v2", File.ReadAllText(Path.Combine(i.Dir, "Minecraft.Windows.exe")));
        Assert.Equal("26.60.99", after.LoaderVersion);
        Assert.True((bool)JsonNode.Parse(File.ReadAllText(Path.Combine(i.Dir, ".levilauncher", "instance.json")))!["enableIsolation"]!);
        Assert.Equal(Path.Combine(_base, "data"), FileLinks.LinkTarget(Path.Combine(i.Dir, "data")));

        // Nothing left to do.
        Assert.True(InstanceStore.Refresh(after).Changed.Empty);
    }

    [Fact]
    public void ACopyTheGameRewroteIsNotStaleButABaseChangeIs()
    {
        var i = Clone();
        string mine = Path.Combine(i.Dir, "PreLoaderConfig.json");
        File.Delete(mine);
        File.WriteAllText(mine, "{ \"rewritten\": true }");
        Assert.DoesNotContain("stale", InstanceStore.Inspect(i.Dir)!.Problems);

        File.WriteAllText(Path.Combine(_base, "PreLoaderConfig.json"), "{ \"v\": 2 }");
        File.SetLastWriteTimeUtc(Path.Combine(_base, "PreLoaderConfig.json"), DateTime.UtcNow.AddMinutes(2));
        var (after, changed) = InstanceStore.Refresh(InstanceStore.Inspect(i.Dir)!);
        Assert.Equal(["PreLoaderConfig.json"], changed.RootFiles);
        Assert.Equal("{ \"v\": 2 }", File.ReadAllText(mine));
        Assert.DoesNotContain("stale", after.Problems);
    }

    [Fact]
    public void RefreshDropsFilesTheBaseDroppedAndCopiesNewFolders()
    {
        var i = Clone();
        File.Delete(Path.Combine(_base, "LeviLauncher.dll"));
        Write("NewFolder/n.txt", "n");
        Write(".levilauncher/instance.json", """{ "name": "My instance", "gameVersion": "1.26.60.06", "enableIsolation": false }""");
        File.WriteAllText(Path.Combine(i.Dir, "logs", "game.log"), "kept");
        var (after, changed) = InstanceStore.Refresh(i);
        Assert.Equal(["LeviLauncher.dll"], changed.Removed);
        Assert.Equal(["NewFolder"], changed.NewDirs);
        Assert.False(File.Exists(Path.Combine(i.Dir, "LeviLauncher.dll")));
        Assert.Equal("n", File.ReadAllText(Path.Combine(i.Dir, "NewFolder", "n.txt")));
        Assert.Equal("1.26.60.06", after.GameVersion);
        Assert.True(File.Exists(Path.Combine(i.Dir, "logs", "game.log")));
        Assert.True(InstanceStore.Refresh(after).Changed.Empty);
    }

    [Fact]
    public void ForceOnANamedCloneKeepsItsBase()
    {
        var i = Clone();
        var other = new FakeLauncher(new LeviLauncherInstance
        {
            Name = "Other", GameVersion = "1.26.60.05", Channel = "release", Type = "gdk", LoaderVersion = "26.60.5",
            Path = _base, IsDefault = true,
        }, new LeviLauncherInstance
        {
            Name = "My instance", GameVersion = "1.26.60.05", Channel = "release", Type = "gdk", LoaderVersion = "26.60.5",
            Path = _base,
        });
        var again = InstanceStore.EnsureClone(new CloneOptions { Name = i.Name, Force = true, Root = _root, Launcher = other });
        Assert.Equal("My instance", again.Base);
        var e = Assert.Throws<CliError>(() =>
            InstanceStore.EnsureClone(new CloneOptions { Name = i.Name, Base = "Other", Root = _root, Launcher = other }));
        Assert.Equal("ERR_OTHER_BASE", e.Code);
    }

    [Fact]
    public void RemovalDeletesReadOnlyLinksWithoutChangingTheBase()
    {
        var i = Clone();
        string baseFile = Path.Combine(_base, "LeviLauncher.dll");
        File.SetAttributes(baseFile, FileAttributes.ReadOnly);
        try
        {
            InstanceStore.Remove(i);
            Assert.False(Directory.Exists(i.Dir));
            Assert.True(File.GetAttributes(baseFile).HasFlag(FileAttributes.ReadOnly));
        }
        finally
        {
            File.SetAttributes(baseFile, FileAttributes.Normal);
        }
    }

    [Fact]
    public void ACloneWhoseBaseIsGoneSaysSo()
    {
        var i = Clone();
        File.Delete(Path.Combine(_base, "Minecraft.Windows.exe"));
        var now = InstanceStore.Inspect(i.Dir)!;
        Assert.Contains("base-missing", now.Problems);
        var e = Assert.Throws<CliError>(() => InstanceStore.Refresh(now));
        Assert.Equal("ERR_BASE_MISSING", e.Code);
    }

    [Fact]
    public void RemoveLeavesTheBaseIntact()
    {
        var i = Clone();
        InstanceStore.Remove(i);
        Assert.False(Directory.Exists(i.Dir));
        Assert.Equal("asset", File.ReadAllText(Path.Combine(_base, "data", "assets", "a.bin")));
        Assert.Equal("inst", File.ReadAllText(Path.Combine(_base, "Installers", "x.txt")));
        Assert.Equal("game", File.ReadAllText(Path.Combine(_base, "Minecraft.Windows.exe")));
    }

    [Fact]
    public void ForceMakesTheCloneAgain()
    {
        var i = Clone();
        File.WriteAllText(Path.Combine(i.Dir, "Minecraft Bedrock", "mine.txt"), "x");
        var again = Clone(force: true);
        Assert.False(File.Exists(Path.Combine(again.Dir, "Minecraft Bedrock", "mine.txt")));
        Assert.Equal("asset", File.ReadAllText(Path.Combine(_base, "data", "assets", "a.bin")));
    }

    [Fact]
    public void AFolderThatIsNoCloneIsNotTouched()
    {
        Directory.CreateDirectory(Path.Combine(_root, "1.26.60.05-3"));
        File.WriteAllText(Path.Combine(_root, "1.26.60.05-3", "keep.txt"), "x");
        var e = Assert.Throws<CliError>(() => Clone());
        Assert.Equal("ERR_NOT_A_CLONE", e.Code);
        Assert.True(File.Exists(Path.Combine(_root, "1.26.60.05-3", "keep.txt")));
    }
}
