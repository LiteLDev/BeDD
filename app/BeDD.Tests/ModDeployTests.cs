using System.Text;
using System.Text.Json.Nodes;
using BeDD.Cli;
using BeDD.Core;

namespace BeDD.Tests;

public sealed class ModDeployTests : IDisposable
{
    private readonly string _tmp = Path.Combine(Path.GetTempPath(), "bedd-mod-" + Guid.NewGuid().ToString("N"));

    public ModDeployTests() => Directory.CreateDirectory(_tmp);

    public void Dispose() => Directory.Delete(_tmp, true);

    private InstanceInfo Instance()
    {
        string dir = Path.Combine(_tmp, "1.26.60.05-9");
        Directory.CreateDirectory(Path.Combine(dir, "mods", "LeviLamina", "config"));
        File.WriteAllText(Path.Combine(dir, "mods", "LeviLamina", "config", "Config.json"),
            """{ "version": 43, "modules": { "crashLogger": { "enabled": true, "builtin": false, "uploadToSentry": true } } }""");
        return new InstanceInfo { Name = "1.26.60.05-9", Dir = dir, Base = "b", BaseDir = _tmp, Slot = 9 };
    }

    private string Source()
    {
        string src = Path.Combine(_tmp, "bin", "BeDD");
        Directory.CreateDirectory(src);
        File.WriteAllText(Path.Combine(src, "BeDD.dll"), "dll");
        File.WriteAllText(Path.Combine(src, "manifest.json"), """{ "name": "BeDD", "version": "2.0.0" }""");
        return src;
    }

    [Fact]
    public void SetParsesJsonOrKeepsText()
    {
        var c = new JsonObject();
        ModDeploy.ApplySet(c, "manager=false");
        ModDeploy.ApplySet(c, "timeScale=2.5");
        ModDeploy.ApplySet(c, "label=hello world");
        ModDeploy.ApplySet(c, "allowOrigins=[\"http://x\"]");
        ModDeploy.ApplySet(c, "nested.deep.key=1");
        ModDeploy.ApplySet(c, "quoted=\"true\"");
        Assert.False((bool)c["manager"]!);
        Assert.Equal(2.5, (double)c["timeScale"]!);
        Assert.Equal("hello world", (string?)c["label"]);
        Assert.Equal("http://x", (string?)c["allowOrigins"]![0]);
        Assert.Equal(1, (int)c["nested"]!["deep"]!["key"]!);
        Assert.Equal("true", (string?)c["quoted"]);
        ModDeploy.ApplyUnset(c, "nested.deep.key");
        ModDeploy.ApplyUnset(c, "label");
        ModDeploy.ApplyUnset(c, "no.such.key");
        Assert.False(c.ContainsKey("label"));
        Assert.Empty(c["nested"]!["deep"]!.AsObject());
        Assert.Throws<CliError>(() => ModDeploy.ApplySet(c, "novalue"));
        Assert.Throws<CliError>(() => ModDeploy.ApplySet(c, "manager.x=1")); // manager is no object
    }

    [Fact]
    public void WorldModeFollowsTheModsRules()
    {
        var c = new JsonObject();
        ModDeploy.ApplyWorldMode(c, "remote", "play.example.net");
        Assert.Equal("Remote", (string?)c["mode"]);
        Assert.Equal("play.example.net", (string?)c["allowHosts"]![0]);

        ModDeploy.ApplyWorldMode(c, "Remote", "127.0.0.1");
        Assert.False(c.ContainsKey("allowHosts"));

        ModDeploy.ApplyWorldMode(c, "clientserver", null);
        Assert.Equal("ClientServer", (string?)c["mode"]);

        Assert.Throws<CliError>(() => ModDeploy.ApplyWorldMode(c, "Remote", null));
        Assert.Throws<CliError>(() => ModDeploy.ApplyWorldMode(c, "ClientOnly", "x"));
        Assert.Throws<CliError>(() => ModDeploy.ApplyWorldMode(c, "Elsewhere", null));
        Assert.Throws<CliError>(() => ModDeploy.ApplyWorldMode(c, null, "x"));
    }

    [Fact]
    public void InstallCopiesTheModWritesOnlyGivenSettingsAndSwitchesTheCrashLogger()
    {
        var i = Instance();
        var r = ModDeploy.Install(i, Source(), new ModSettings { Set = ["manager=false"], WorldMode = "ClientOnly" });
        string mod = Path.Combine(i.Dir, "mods", "BeDD");
        Assert.True(File.Exists(Path.Combine(mod, "BeDD.dll")));
        Assert.True(File.Exists(Path.Combine(mod, "manifest.json")));
        Assert.False(File.Exists(Path.Combine(mod, "BeDD.pdb")));
        Assert.Equal("2.0.0", (string?)r["version"]);
        Assert.Equal("switched", (string?)r["crashLogger"]);

        var config = JsonNode.Parse(File.ReadAllText(Path.Combine(mod, "config", "config.json")))!.AsObject();
        Assert.Equal(2, config.Count);
        Assert.False((bool)config["manager"]!);
        Assert.Equal("ClientOnly", (string?)config["mode"]);

        var ll = JsonNode.Parse(File.ReadAllText(Path.Combine(i.Dir, "mods", "LeviLamina", "config", "Config.json")))!;
        Assert.True((bool)ll["modules"]!["crashLogger"]!["builtin"]!);
        Assert.True((bool)ll["modules"]!["crashLogger"]!["uploadToSentry"]!);
        Assert.Equal(43, (int)ll["version"]!);
        Assert.Equal("builtin", ModDeploy.SwitchCrashLogger(i.ModsDir));

        // config changes the file; a reinstall starts from the given settings again.
        var after = ModDeploy.Configure(i, new ModSettings { Set = ["trace=true"], Unset = ["manager"] });
        Assert.True((bool)after["trace"]!);
        Assert.False(after.ContainsKey("manager"));
        Assert.Equal(after.ToJsonString(), ModDeploy.ReadConfig(i).ToJsonString());
        ModDeploy.Install(i, Source(), new ModSettings());
        Assert.Empty(ModDeploy.ReadConfig(i));

        Assert.True(ModDeploy.Uninstall(i));
        Assert.False(Directory.Exists(mod));
        Assert.False(ModDeploy.Uninstall(i));
        Assert.Equal("ERR_MOD_NOT_INSTALLED", Assert.Throws<CliError>(() => ModDeploy.Configure(i, new ModSettings())).Code);
    }

    [Fact]
    public void CrashLoggerIsLeftAloneWhenOffOrUnconfigured()
    {
        var i = Instance();
        string file = Path.Combine(i.ModsDir, "LeviLamina", "config", "Config.json");
        File.WriteAllText(file, """{ "modules": { "crashLogger": { "enabled": false } } }""");
        Assert.Equal("disabled", ModDeploy.SwitchCrashLogger(i.ModsDir));
        File.Delete(file);
        Assert.Equal("noConfig", ModDeploy.SwitchCrashLogger(i.ModsDir));
    }

    [Fact]
    public void FindsTheModNextToTheAppOrUpFromTheCurrentFolder()
    {
        string src = Source();
        string deep = Path.Combine(_tmp, "a", "b");
        Directory.CreateDirectory(deep);
        string noApp = Path.Combine(_tmp, "app");
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("BEDD_MOD_DIR")))
        {
            Assert.Equal(src, ModDeploy.FindSource(null, cwd: deep, appDir: noApp));
            string release = Path.Combine(noApp, "mod", "BeDD");
            Directory.CreateDirectory(release);
            File.WriteAllText(Path.Combine(release, "BeDD.dll"), "dll");
            Assert.Equal(release, ModDeploy.FindSource(null, cwd: deep, appDir: noApp));
        }
        Assert.Equal(src, ModDeploy.FindSource(src));
        Assert.Throws<CliError>(() => ModDeploy.FindSource(deep));
    }

    // The mod next to an installed BeDD.exe (appDir\mod\BeDD).
    private string Release(string version, string dll, string? name = null)
    {
        string app = Path.Combine(_tmp, name ?? "app-" + version);
        string mod = Path.Combine(app, "mod", "BeDD");
        Directory.CreateDirectory(mod);
        File.WriteAllText(Path.Combine(mod, "BeDD.dll"), dll);
        File.WriteAllText(Path.Combine(mod, "manifest.json"), $$"""{ "name": "BeDD", "version": "{{version}}" }""");
        return app;
    }

    [Fact]
    public void EnsureCurrentInstallsAMissingModAndUpdatesAnOldOneKeepingItsConfig()
    {
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("BEDD_MOD_DIR")))
            return; // the variable would pick the source
        var i = Instance();
        string cwd = Path.Combine(_tmp, "cwd");
        Directory.CreateDirectory(cwd);
        string mod = Path.Combine(i.Dir, "mods", "BeDD");

        // Nothing to compare with: nothing changes.
        Assert.Equal("no-source", ModDeploy.EnsureCurrent(i, cwd: cwd, appDir: Path.Combine(_tmp, "none")).Action);
        Assert.False(Directory.Exists(mod));

        string v1 = Release("2.0.0", "dll 1");
        var r = ModDeploy.EnsureCurrent(i, cwd: cwd, appDir: v1);
        Assert.Equal(("installed", "2.0.0"), (r.Action, r.To));
        Assert.Equal("dll 1", File.ReadAllText(Path.Combine(mod, "BeDD.dll")));
        Assert.Equal("current", ModDeploy.EnsureCurrent(i, cwd: cwd, appDir: v1).Action);

        ModDeploy.Configure(i, new ModSettings { Set = ["manager=false"] });
        File.WriteAllText(Path.Combine(mod, "BeDD.pdb"), "old pdb"); // a build without a pdb leaves none behind
        string v2 = Release("2.1.0", "dll 2");
        r = ModDeploy.EnsureCurrent(i, cwd: cwd, appDir: v2);
        Assert.Equal(("updated", "2.0.0", "2.1.0"), (r.Action, r.From, r.To));
        Assert.Equal("dll 2", File.ReadAllText(Path.Combine(mod, "BeDD.dll")));
        Assert.Equal("2.1.0", Cloner.ModVersion(mod));
        Assert.False(File.Exists(Path.Combine(mod, "BeDD.pdb")));
        Assert.False((bool)ModDeploy.ReadConfig(i)["manager"]!);

        // Another build of the same version (a development build) is a difference too.
        string rebuilt = Release("2.1.0", "dll 2 rebuilt", "app-rebuilt");
        Assert.Equal("updated", ModDeploy.EnsureCurrent(i, cwd: cwd, appDir: rebuilt).Action);
        Assert.Equal("dll 2 rebuilt", File.ReadAllText(Path.Combine(mod, "BeDD.dll")));
    }

    [Fact]
    public void EnsureCurrentFollowsTheFolderOfAnInstallFrom()
    {
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("BEDD_MOD_DIR")))
            return;
        var i = Instance();
        string cwd = Path.Combine(_tmp, "cwd");
        Directory.CreateDirectory(cwd);
        string mod = Path.Combine(i.Dir, "mods", "BeDD");
        string pinned = Path.Combine(Release("3.0.0", "pinned dll", "pinned-app"), "mod", "BeDD");
        string other = Release("2.0.0", "default dll", "default-app");

        ModDeploy.Install(i, pinned, new ModSettings(), pin: true);
        // The default source differs, but the install named its folder: it is left alone.
        Assert.Equal("current", ModDeploy.EnsureCurrent(i, cwd: cwd, appDir: other).Action);
        Assert.Equal("pinned dll", File.ReadAllText(Path.Combine(mod, "BeDD.dll")));

        // A new build in the pinned folder is taken up.
        File.WriteAllText(Path.Combine(pinned, "BeDD.dll"), "pinned dll 2");
        Assert.Equal("updated", ModDeploy.EnsureCurrent(i, cwd: cwd, appDir: other).Action);
        Assert.Equal("pinned dll 2", File.ReadAllText(Path.Combine(mod, "BeDD.dll")));

        // The pinned folder gone: nothing changes.
        Directory.Delete(pinned, true);
        Assert.Equal("pinned-missing", ModDeploy.EnsureCurrent(i, cwd: cwd, appDir: other).Action);
        Assert.Equal("pinned dll 2", File.ReadAllText(Path.Combine(mod, "BeDD.dll")));

        // An install without --from ends the pin.
        ModDeploy.Install(i, Path.Combine(other, "mod", "BeDD"), new ModSettings());
        Assert.False(File.Exists(Path.Combine(mod, ModDeploy.PinFile)));
    }
}

public sealed class WorldTests : IDisposable
{
    private readonly string _tmp = Path.Combine(Path.GetTempPath(), "bedd-world-" + Guid.NewGuid().ToString("N"));

    public WorldTests() => Directory.CreateDirectory(_tmp);

    public void Dispose() => Directory.Delete(_tmp, true);

    private static void Tag(MemoryStream s, byte type, string name, int value)
    {
        var nb = Encoding.UTF8.GetBytes(name);
        s.WriteByte(type);
        s.WriteByte((byte)nb.Length);
        s.WriteByte(0);
        s.Write(nb);
        if (type == 1)
            s.WriteByte((byte)value);
        else
            s.Write(BitConverter.GetBytes(value));
    }

    // A level.dat: the 8-byte header, then a compound with the tags.
    private static byte[] LevelDat()
    {
        var s = new MemoryStream();
        s.Write(new byte[] { 10, 0, 0, 0, 0, 0, 0, 0 });
        s.Write(new byte[] { 10, 0, 0 });
        Tag(s, 1, "MultiplayerGame", 1);
        Tag(s, 1, "MultiplayerGameIntent", 1);
        Tag(s, 1, "LANBroadcast", 1);
        Tag(s, 1, "LANBroadcastIntent", 0);
        Tag(s, 3, "XBLBroadcastIntent", 3);
        Tag(s, 3, "PlatformBroadcastIntent", 3);
        Tag(s, 3, "SpawnX", 3);
        s.WriteByte(0);
        return s.ToArray();
    }

    [Fact]
    public void ClearsTheMultiplayerTagsOnly()
    {
        var b = LevelDat();
        var changed = Worlds.PatchLevelDat(b);
        Assert.Equal(["MultiplayerGame", "MultiplayerGameIntent", "LANBroadcast", "XBLBroadcastIntent", "PlatformBroadcastIntent"], changed);
        var again = (byte[])b.Clone();
        Assert.Empty(Worlds.PatchLevelDat(again));
        // SpawnX keeps its value: the last 5 bytes are the int 3 and the end tag.
        Assert.Equal(3, BitConverter.ToInt32(b, b.Length - 5));
        // "MultiplayerGame" does not match inside "MultiplayerGameIntent" (the length differs).
        int intent = IndexOf(b, "MultiplayerGameIntent");
        Assert.Equal(0, b[intent + "MultiplayerGameIntent".Length]);
    }

    private static int IndexOf(byte[] b, string s) => b.AsSpan().IndexOf(Encoding.UTF8.GetBytes(s));

    [Fact]
    public void FindsWorldsAndPatchesBothFiles()
    {
        string w = Path.Combine(_tmp, "Minecraft Bedrock", "Users", "123", "games", "com.mojang", "minecraftWorlds", "Showcase");
        Directory.CreateDirectory(w);
        File.WriteAllBytes(Path.Combine(w, "level.dat"), LevelDat());
        File.WriteAllBytes(Path.Combine(w, "level.dat_old"), LevelDat());
        Assert.Equal([w], Worlds.Find(_tmp, "showcase"));
        Assert.Equal([w], Worlds.Recent(_tmp, TimeSpan.FromMinutes(5)));
        Assert.Equal(5, Worlds.SetSinglePlayer(w).Count);
        Assert.Empty(Worlds.PatchLevelDat(File.ReadAllBytes(Path.Combine(w, "level.dat_old"))));
        Assert.Empty(Worlds.SetSinglePlayer(w));
    }
}
