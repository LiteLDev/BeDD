using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using BeDD.Cli;

namespace BeDD.Core;

// A BeDD game instance: a clone of a LeviLauncher instance under Paths.Instances, named
// <base gameVersion>-<slot>. Slot 1 is a clone too: BeDD never runs games in the user's LeviLauncher instance.
public sealed record InstanceInfo
{
    // Folder name under Paths.Instances, e.g. "1.26.60.05-2".
    public required string Name { get; init; }
    public required string Dir { get; init; }
    public string ModsDir => Path.Combine(Dir, "mods");
    // The LeviLauncher instance it is cloned from (its LeviLauncher name) and that one's folder.
    public required string Base { get; init; }
    public required string BaseDir { get; init; }
    public required int Slot { get; init; }
    public string GameVersion { get; init; } = "";
    public string? LoaderVersion { get; init; }
    // What it still lacks or what is wrong; empty when ready:
    //   missing       the clone has no game exe
    //   base-missing  the LeviLauncher instance it was cloned from is gone
    //   stale         the base changed since (root files, metadata, LeviLamina): instance refresh
    //   junction      data or Installers does not point to the base's: instance refresh
    //   leviLamina    no mods\LeviLamina
    //   mod-missing   no mods\BeDD (mod install)
    public IReadOnlyList<string> Problems { get; init; } = [];
    public DateTimeOffset? CreatedAt { get; init; }
    // The BeDD mod's version in mods\BeDD, null when it is not installed.
    public string? ModVersion { get; init; }

    public string LogsDir => Path.Combine(Dir, "logs");
    // The game data (LeviLauncher's "data" kind): worlds, settings, accounts of this instance only.
    public string DataDir => Path.Combine(Dir, "Minecraft Bedrock");

    public JsonObject ToJson() => new()
    {
        ["name"] = Name,
        ["slot"] = Slot,
        ["dir"] = Dir,
        ["base"] = Base,
        ["baseDir"] = BaseDir,
        ["gameVersion"] = GameVersion,
        ["loaderVersion"] = LoaderVersion,
        ["modVersion"] = ModVersion,
        ["createdAt"] = CreatedAt?.ToString("o", System.Globalization.CultureInfo.InvariantCulture),
        ["ready"] = Problems.Count == 0,
        ["problems"] = new JsonArray(Problems.Select(p => (JsonNode?)p).ToArray()),
    };
}

// .bedd\clone.json in a clone.
public sealed record CloneRecord
{
    public string Base { get; init; } = "";
    public string BaseDir { get; init; } = "";
    public string BaseGameVersion { get; init; } = "";
    public int Slot { get; init; }
    public DateTimeOffset CreatedAt { get; init; }
    // The root files it got from the base (to drop those the base drops later).
    public List<string> RootFiles { get; init; } = [];
    // The base's files the clone has copies of, as they were when copied.
    public Dictionary<string, FileStamp> Copies { get; init; } = new(StringComparer.OrdinalIgnoreCase);
}

public sealed record FileStamp
{
    public long Length { get; init; }
    public DateTime LastWriteUtc { get; init; }
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase, WriteIndented = true)]
[JsonSerializable(typeof(CloneRecord))]
internal sealed partial class InstancesJson : JsonSerializerContext;

internal static class JsonText
{
    // Text other programs read (the game's loader, LeviLamina): non-ASCII characters (an instance
    // named in Chinese) as they are, not as \u escapes.
    private static readonly JsonWriterOptions Options = new()
    {
        Indented = true,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    // UTF-8 without BOM, indented.
    public static byte[] Indented(JsonNode node)
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms, Options))
            node.WriteTo(w);
        return ms.ToArray();
    }

    public static byte[] Indented<T>(T value, System.Text.Json.Serialization.Metadata.JsonTypeInfo<T> type)
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms, Options))
            JsonSerializer.Serialize(w, value, type);
        return ms.ToArray();
    }
}

// One BeDD setup at a time on the PC (cloning, refreshing, removing, deploying the mod): several
// test runs may start at once. The same mutex the PowerShell client used.
public sealed class SetupLock : IDisposable
{
    public const string Name = @"Global\BeDD.Setup";
    public static readonly TimeSpan DefaultTimeout = TimeSpan.FromMinutes(15);

    private readonly Mutex _mutex;

    private SetupLock(Mutex mutex) => _mutex = mutex;

    // Tests use a mutex of their own, so that they neither wait for nor block a real setup.
    internal static string MutexName { get; set; } = Name;

    public static SetupLock Acquire(TimeSpan? timeout = null, Action<string>? waiting = null)
    {
        var mutex = new Mutex(false, MutexName);
        bool owned;
        try
        {
            owned = mutex.WaitOne(TimeSpan.Zero);
            if (!owned)
            {
                waiting?.Invoke("waiting for another BeDD setup to finish");
                owned = mutex.WaitOne(timeout ?? DefaultTimeout);
            }
        }
        catch (AbandonedMutexException)
        {
            owned = true; // its owner died; the state on disk is checked anyway
        }
        if (!owned)
        {
            mutex.Dispose();
            throw CliError.Conflict("ERR_BUSY", "another BeDD setup did not finish in time");
        }
        return new SetupLock(mutex);
    }

    public void Dispose()
    {
        _mutex.ReleaseMutex();
        _mutex.Dispose();
    }
}

public sealed class CloneOptions
{
    // An instance folder name; null = the clone of the base instance for Slot.
    public string? Name { get; init; }
    public int Slot { get; init; } = 1;
    // The LeviLauncher instance to clone (else BEDD_INSTANCE, the default one, or the first usable).
    public string? Base { get; init; }
    // Rebuild the clone from scratch.
    public bool Force { get; init; }
    public Action<string>? Progress { get; init; }
    public Action<string>? Warn { get; init; }
    // Tests: another instances folder, a fake LeviLauncher, a fake hard-link call.
    public string? Root { get; init; }
    public ILeviLauncher? Launcher { get; init; }
    public Func<string, string, int>? HardLink { get; init; }
}

public static partial class InstanceStore
{
    public static string FolderName(string gameVersion, int slot) => $"{gameVersion}-{slot}";

    [GeneratedRegex(@"^(?<version>.+)-(?<slot>[0-9]+)$")]
    private static partial Regex NamePattern();

    // Every BeDD instance under Paths.Instances.
    public static IReadOnlyList<InstanceInfo> List() => List(Paths.Instances);

    public static IReadOnlyList<InstanceInfo> List(string root)
    {
        if (!Directory.Exists(root))
            return [];
        var result = new List<InstanceInfo>();
        foreach (var dir in Directory.EnumerateDirectories(root))
        {
            var i = Inspect(dir);
            if (i is not null)
                result.Add(i);
        }
        result.Sort((a, b) => a.Slot != b.Slot ? a.Slot.CompareTo(b.Slot) : string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
        return result;
    }

    // The instance named name (a folder under Paths.Instances), or the clone of the base instance for
    // slot when name is null. Null when it does not exist yet.
    public static InstanceInfo? Resolve(string? name, int slot = 1) => Resolve(Paths.Instances, name, slot);

    // baseName: only a clone of that LeviLauncher instance (null = BEDD_INSTANCE, when set).
    public static InstanceInfo? Resolve(string root, string? name, int slot, string? baseName = null)
    {
        if (name is not null)
        {
            CheckName(name);
            return Inspect(Path.Combine(root, name));
        }
        if (!Directory.Exists(root))
            return null;
        // The clone for the slot: of the base asked for, of the game BeDD is built for, the newest.
        // Only its clone.json is read until one is picked.
        var wanted = string.IsNullOrWhiteSpace(baseName) ? Environment.GetEnvironmentVariable("BEDD_INSTANCE") : baseName;
        var pick = Directory.EnumerateDirectories(root)
            .Select(d => (Dir: d, Rec: Cloner.ReadRecord(d)))
            .Where(x => x.Rec is not null && x.Rec.Slot == slot && BaseSelection.MatchesGame(x.Rec.BaseGameVersion))
            .Where(x => string.IsNullOrWhiteSpace(wanted) || string.Equals(x.Rec!.Base, wanted.Trim(), StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(x => x.Rec!.CreatedAt)
            .FirstOrDefault();
        return pick.Dir is null ? null : Inspect(pick.Dir);
    }

    // Resolve, or a not-found error with a hint.
    public static InstanceInfo Require(string? name, int slot = 1)
    {
        var i = Resolve(name, slot);
        if (i is not null)
            return i;
        throw name is null
            ? CliError.NotFound("ERR_INSTANCE_NOT_FOUND", $"no instance for slot {slot}", $"{Dispatcher.ProgramName} instance clone --slot {slot}")
            : CliError.NotFound("ERR_INSTANCE_NOT_FOUND", $"no instance '{name}'", $"{Dispatcher.ProgramName} instance list");
    }

    private static void CheckName(string name)
    {
        if (name.Length == 0 || name is "." or ".." || name.IndexOfAny(['\\', '/', ':']) >= 0 || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw CliError.Usage($"'{name}' is not an instance name", $"{Dispatcher.ProgramName} instance list");
    }

    // The instance in dir, or null when dir is not a BeDD clone.
    public static InstanceInfo? Inspect(string dir)
    {
        var rec = Cloner.ReadRecord(dir);
        if (rec is null)
            return null;
        var problems = new List<string>();
        if (!File.Exists(Path.Combine(dir, "Minecraft.Windows.exe")))
            problems.Add("missing");
        if (!File.Exists(Path.Combine(rec.BaseDir, "Minecraft.Windows.exe")))
        {
            problems.Add("base-missing");
        }
        else
        {
            var diff = Cloner.Compare(rec.BaseDir, dir);
            if (diff.Stale)
                problems.Add("stale");
            if (diff.Junctions.Count > 0)
                problems.Add("junction");
        }
        string ll = Path.Combine(dir, "mods", "LeviLamina");
        if (!File.Exists(Path.Combine(ll, "LeviLamina.dll")))
            problems.Add("leviLamina");
        string bedd = Path.Combine(dir, "mods", "BeDD");
        bool mod = File.Exists(Path.Combine(bedd, "BeDD.dll"));
        if (!mod)
            problems.Add("mod-missing");
        return new InstanceInfo
        {
            Name = Path.GetFileName(Path.TrimEndingDirectorySeparator(dir)),
            Dir = Path.GetFullPath(dir),
            Base = rec.Base,
            BaseDir = rec.BaseDir,
            Slot = rec.Slot,
            GameVersion = rec.BaseGameVersion,
            LoaderVersion = Cloner.ModVersion(ll),
            ModVersion = mod ? Cloner.ModVersion(bedd) ?? "" : null,
            CreatedAt = rec.CreatedAt,
            Problems = problems,
        };
    }

    // Resolve, creating or refreshing the clone from its LeviLauncher base instance first.
    public static InstanceInfo EnsureClone(string? name, int slot = 1, Action<string>? progress = null) =>
        EnsureClone(new CloneOptions { Name = name, Slot = slot, Progress = progress });

    public static InstanceInfo EnsureClone(CloneOptions o)
    {
        if (o.Slot < 1)
            throw CliError.Usage($"slot {o.Slot} is not a slot (1 or more)");
        string root = o.Root ?? Paths.Instances;
        using var _ = SetupLock.Acquire(waiting: o.Progress);
        Directory.CreateDirectory(root);
        var cloner = new Cloner { Progress = o.Progress, Warn = o.Warn, HardLink = o.HardLink ?? FileLinks.TryHardLink };

        int slot = o.Slot;
        string? baseName = o.Base;
        string name;
        if (o.Name is not null)
        {
            CheckName(o.Name);
            name = o.Name;
            var existing = Inspect(Path.Combine(root, name));
            if (existing is not null)
            {
                if (!string.IsNullOrWhiteSpace(o.Base) && !string.Equals(o.Base.Trim(), existing.Base, StringComparison.OrdinalIgnoreCase) && !o.Force)
                    throw CliError.Conflict("ERR_OTHER_BASE", $"{name} is a clone of '{existing.Base}', not '{o.Base}'",
                        $"{Dispatcher.ProgramName} instance clone --slot {existing.Slot} --base \"{o.Base}\" --force");
                if (!o.Force)
                    return RefreshLocked(existing, cloner, o, throwIfRunning: false);
                slot = existing.Slot;
                // Made again from the base it was made from, unless another one is asked for.
                baseName = string.IsNullOrWhiteSpace(o.Base) ? existing.Base : o.Base;
            }
            else
            {
                var m = NamePattern().Match(name);
                if (!m.Success)
                    throw CliError.NotFound("ERR_INSTANCE_NOT_FOUND", $"no instance '{name}' (a new one is named <game version>-<slot>)",
                        $"{Dispatcher.ProgramName} instance clone --slot {o.Slot}");
                slot = int.Parse(m.Groups["slot"].Value, System.Globalization.CultureInfo.InvariantCulture);
            }
        }
        else if (!o.Force)
        {
            // The clone the slot already has: no need to ask LeviLauncher.
            var existing = Resolve(root, null, slot, o.Base);
            if (existing is not null)
                return RefreshLocked(existing, cloner, o, throwIfRunning: false);
            name = "";
        }
        else
        {
            name = "";
        }

        var launcher = o.Launcher ?? LeviLauncherCli.Find();
        string command = launcher is LeviLauncherCli cli ? cli.Command : "LeviLauncher.exe";
        var b = BaseSelection.Select(launcher.ListInstances(), baseName, command);
        if (BaseSelection.LoaderOlder(b))
            o.Warn?.Invoke($"'{b.Name}' has LeviLamina {b.LoaderVersion}, older than {BuildInfo.LeviLaminaVersion} BeDD is built for " +
                $"(update it: {command} loader install \"{b.Name}\")");
        string expected = FolderName(b.GameVersion, slot);
        if (name.Length == 0)
            name = expected;
        else if (!string.Equals(name, expected, StringComparison.OrdinalIgnoreCase) && Inspect(Path.Combine(root, name)) is null)
            throw CliError.Usage($"a clone of '{b.Name}' for slot {slot} is named {expected}, not {name}");
        string dir = Path.Combine(root, name);

        var rec = Cloner.ReadRecord(dir);
        if (rec is null && (Directory.Exists(dir) || File.Exists(dir)))
            throw CliError.Conflict("ERR_NOT_A_CLONE", $"{dir} exists but is not a BeDD instance; move it away first");
        if (rec is not null && !o.Force)
        {
            if (!SamePath(rec.BaseDir, b.Path))
                throw CliError.Conflict("ERR_OTHER_BASE", $"{name} is a clone of '{rec.Base}', not '{b.Name}'",
                    $"{Dispatcher.ProgramName} instance clone --slot {slot} --base \"{b.Name}\" --force");
            return RefreshLocked(Inspect(dir)!, cloner, o, throwIfRunning: false);
        }
        if (rec is not null)
        {
            ThrowIfRunning(name, dir);
            o.Progress?.Invoke($"removing {dir} to clone it again");
            FileLinks.DeleteTree(dir, last: Cloner.MetaDir);
        }
        cloner.Create(b.Name, b.Path, b.GameVersion, slot, dir);
        o.Progress?.Invoke($"{name} ready at {dir}");
        return Inspect(dir)!;
    }

    private static bool SamePath(string a, string b) =>
        string.Equals(Path.GetFullPath(a).TrimEnd('\\'), Path.GetFullPath(b).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);

    private static CliError Running(string name, IReadOnlyList<int> pids) =>
        CliError.Conflict("ERR_RUNNING", $"{name} has a game running (pid {string.Join(", ", pids)})",
            $"{Dispatcher.ProgramName} process stop {name}");

    // Files of an instance whose game runs are in use: setup refuses to touch them.
    public static void ThrowIfRunning(string name, string dir)
    {
        var running = FileLinks.ProcessesIn(dir);
        if (running.Count > 0)
            throw Running(name, running);
    }

    private static InstanceInfo RefreshLocked(InstanceInfo i, Cloner cloner, CloneOptions o, bool throwIfRunning) =>
        RefreshCore(i, cloner, o.Progress, o.Warn, throwIfRunning).Instance;

    // With the setup lock held. A clone whose game runs is left alone (its files are in use).
    private static (InstanceInfo Instance, CloneDiff Changed) RefreshCore(InstanceInfo i, Cloner cloner,
        Action<string>? progress, Action<string>? warn, bool throwIfRunning)
    {
        if (i.Problems.Contains("base-missing"))
            throw CliError.Failure("ERR_BASE_MISSING", $"the LeviLauncher instance {i.Name} was cloned from is gone ({i.BaseDir})",
                $"{Dispatcher.ProgramName} instance clone --slot {i.Slot} --force");
        var running = FileLinks.ProcessesIn(i.Dir);
        if (running.Count > 0)
        {
            if (throwIfRunning)
                throw Running(i.Name, running);
            progress?.Invoke($"{i.Name} has a game running: not refreshing it");
            return (i, new CloneDiff());
        }
        var diff = cloner.Refresh(i.BaseDir, i.Dir);
        // LeviLamina writes its config at the game's first start, and a new LeviLamina comes with its
        // default one: switch its crash logger whenever the mod is in (a no-op once switched).
        if (Directory.Exists(Path.Combine(i.ModsDir, "BeDD")))
            ModDeploy.SwitchCrashLogger(i.ModsDir, warn);
        if (!diff.Empty)
            progress?.Invoke($"{i.Name} refreshed from '{i.Base}' ({diff.Count} item(s))");
        return (Inspect(i.Dir)!, diff);
    }

    // Brings a clone up to date with its base; fails while its game runs.
    public static (InstanceInfo Instance, CloneDiff Changed) Refresh(InstanceInfo i, Action<string>? progress = null, Action<string>? warn = null,
        Func<string, string, int>? hardLink = null)
    {
        using var _ = SetupLock.Acquire(waiting: progress);
        var cloner = new Cloner { Progress = progress, Warn = warn, HardLink = hardLink ?? FileLinks.TryHardLink };
        return RefreshCore(Inspect(i.Dir) ?? i, cloner, progress, warn, throwIfRunning: true);
    }

    // Deletes a clone: its own files, and the junctions and hard links without touching the base.
    public static void Remove(InstanceInfo i)
    {
        using var _ = SetupLock.Acquire();
        ThrowIfRunning(i.Name, i.Dir);
        FileLinks.DeleteTree(i.Dir, last: Cloner.MetaDir);
    }

    // Running games of the instance (Minecraft.Windows.exe inside its folder).
    public static IReadOnlyList<int> RunningGames(InstanceInfo i) => FileLinks.ProcessesIn(i.Dir);
}
