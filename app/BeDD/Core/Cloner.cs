using System.Text.Json;
using System.Text.Json.Nodes;
using BeDD.Cli;

namespace BeDD.Core;

// Makes and refreshes the clone of a LeviLauncher instance, BeDD's game instance. The base is only
// ever read. A clone:
//   - root files: hard links to the base's (copies when the clone is on another volume or the file
//     system has no hard links); small config files (*.json, *.xml, *.config) are copied: the game
//     rewrites some of them as it starts (appxmanifest.xml), which through a hard link could write
//     into the base. A copy counts as stale only when the base's file changed since it was made;
//   - data, Installers: junctions to the base's (the game's assets, read-only);
//   - Minecraft Bedrock, .levilauncher, .lip: copies (the game data is the clone's own; isolation
//     forced on in the metadata LeviLauncher.dll reads);
//   - logs, mods: fresh, with only mods\LeviLamina (and mods\CrashLogger) copied;
//   - .bedd\clone.json: where it comes from.
// Nothing in a clone's root is ever written in place: a stale file is deleted and linked again.
public sealed class Cloner
{
    public static readonly string[] Junctions = ["data", "Installers"];
    public static readonly string[] FreshDirs = ["logs", "mods"];
    // Mods that come with the loader and go into every clone.
    public static readonly string[] LoaderMods = ["LeviLamina", "CrashLogger"];
    public const string MetaDir = ".bedd";

    // Creates a hard link; returns 0 or the Win32 error. Tests replace it to simulate another volume.
    public Func<string, string, int> HardLink { get; init; } = FileLinks.TryHardLink;
    public Action<string>? Progress { get; init; }
    public Action<string>? Warn { get; init; }

    private bool _copyWarned;

    public static string CloneJsonPath(string dir) => Path.Combine(dir, MetaDir, "clone.json");

    // Root folders of the base that are not part of a clone at all.
    private static bool SkipDir(string name) =>
        name.Equals("mods-disabled", StringComparison.OrdinalIgnoreCase) ||
        name.StartsWith("mods.", StringComparison.OrdinalIgnoreCase) ||
        name.Equals(MetaDir, StringComparison.OrdinalIgnoreCase);

    // Root files of the base that are not part of a clone: LeviLauncher's backups of the unpatched
    // game exe (300 MB each, never run).
    private static bool SkipFile(string name) =>
        name.EndsWith(".bak", StringComparison.OrdinalIgnoreCase) || name.EndsWith(".orig", StringComparison.OrdinalIgnoreCase) ||
        name.EndsWith(".bedd-tmp", StringComparison.OrdinalIgnoreCase);

    // Root files copied instead of linked.
    private static readonly string[] CopiedExtensions = [".json", ".xml", ".config", ".ini", ".txt"];

    private static bool CopiedFile(string name) => CopiedExtensions.Any(e => name.EndsWith(e, StringComparison.OrdinalIgnoreCase));

    private static FileStamp Stamp(FileInfo f) => new() { Length = f.Length, LastWriteUtc = f.LastWriteTimeUtc };

    // The base's copied root files as they are now.
    private static Dictionary<string, FileStamp> CopyStamps(string baseDir) =>
        BaseRootFiles(baseDir).Where(f => CopiedFile(f.Name) && !IsMetadata(f.Name))
            .ToDictionary(f => f.Name, Stamp, StringComparer.OrdinalIgnoreCase);

    // Instance metadata LeviLauncher.dll reads (the newer of the two wins): copied with
    // enableIsolation forced on, with the base's time kept so that the newer one stays the newer one.
    private static bool IsMetadata(string relative) =>
        relative.Equals("version.json", StringComparison.OrdinalIgnoreCase) ||
        relative.Equals(Path.Combine(".levilauncher", "instance.json"), StringComparison.OrdinalIgnoreCase);

    // Creates the clone at dest (which must not exist) from the base at baseDir.
    public void Create(string baseName, string baseDir, string baseGameVersion, int slot, string dest)
    {
        if (!File.Exists(Path.Combine(baseDir, "Minecraft.Windows.exe")))
            throw CliError.Failure("ERR_BASE_MISSING", $"the LeviLauncher instance '{baseName}' has no game at {baseDir}");
        if (Directory.Exists(dest) || File.Exists(dest))
            throw CliError.Conflict("ERR_EXISTS", $"{dest} already exists");
        // Built under a temporary name and renamed when complete: a half-made clone never looks like one.
        string partial = dest + ".partial";
        if (Directory.Exists(partial))
            FileLinks.DeleteTree(partial);
        Directory.CreateDirectory(partial);
        try
        {
            Progress?.Invoke($"cloning '{baseName}' into {dest}");
            var rootFiles = new List<string>();
            foreach (var f in BaseRootFiles(baseDir))
            {
                PlaceRootFile(f, Path.Combine(partial, f.Name));
                rootFiles.Add(f.Name);
            }
            foreach (var d in new DirectoryInfo(baseDir).EnumerateDirectories("*", new EnumerationOptions { AttributesToSkip = 0 }))
            {
                string target = Path.Combine(partial, d.Name);
                if (SkipDir(d.Name) || FreshDirs.Contains(d.Name, StringComparer.OrdinalIgnoreCase))
                    continue;
                if (Junctions.Contains(d.Name, StringComparer.OrdinalIgnoreCase))
                {
                    FileLinks.CreateJunction(target, d.FullName);
                    continue;
                }
                if (FileLinks.IsLink(d))
                    continue; // a link of the user's own; not ours to follow
                Progress?.Invoke($"copying {d.Name}");
                FileLinks.CopyTree(d.FullName, target);
            }
            ForceIsolation(baseDir, partial);
            foreach (var name in FreshDirs)
                Directory.CreateDirectory(Path.Combine(partial, name));
            foreach (var m in LoaderMods)
            {
                string from = Path.Combine(baseDir, "mods", m);
                if (Directory.Exists(from))
                    FileLinks.CopyTree(from, Path.Combine(partial, "mods", m));
            }
            WriteRecord(partial, new CloneRecord
            {
                Base = baseName,
                BaseDir = Path.GetFullPath(baseDir),
                BaseGameVersion = baseGameVersion,
                Slot = slot,
                CreatedAt = DateTimeOffset.Now,
                RootFiles = rootFiles,
                Copies = CopyStamps(baseDir),
            });
            Directory.Move(partial, dest);
        }
        catch
        {
            try
            {
                FileLinks.DeleteTree(partial);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Warn?.Invoke($"could not remove the half-made clone {partial}: {e.Message}");
            }
            throw;
        }
    }

    private static IEnumerable<FileInfo> BaseRootFiles(string baseDir) =>
        new DirectoryInfo(baseDir).EnumerateFiles("*", new EnumerationOptions { AttributesToSkip = 0 }).Where(f => !SkipFile(f.Name));

    // Root folders of the base a clone has a copy of (everything but the junctions, the fresh and
    // the skipped ones).
    private static IEnumerable<DirectoryInfo> BaseCopiedDirs(string baseDir) =>
        new DirectoryInfo(baseDir).EnumerateDirectories("*", new EnumerationOptions { AttributesToSkip = 0 })
            .Where(d => !SkipDir(d.Name) && !FreshDirs.Contains(d.Name, StringComparer.OrdinalIgnoreCase) &&
                !Junctions.Contains(d.Name, StringComparer.OrdinalIgnoreCase) && !FileLinks.IsLink(d));

    // The game version the base's metadata names now (LeviLauncher may update an instance in place).
    public static string? BaseGameVersion(string baseDir)
    {
        foreach (var rel in new[] { Path.Combine(".levilauncher", "instance.json"), "version.json" })
        {
            try
            {
                var v = (string?)JsonNode.Parse(File.ReadAllBytes(Path.Combine(baseDir, rel)))?["gameVersion"];
                if (!string.IsNullOrWhiteSpace(v))
                    return v;
            }
            catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException or InvalidOperationException)
            {
            }
        }
        return null;
    }

    public static void WriteRecord(string dir, CloneRecord record)
    {
        Directory.CreateDirectory(Path.Combine(dir, MetaDir));
        FileLinks.ReplaceFile(CloneJsonPath(dir), JsonText.Indented(record, InstancesJson.Default.CloneRecord));
    }

    public static CloneRecord? ReadRecord(string dir)
    {
        try
        {
            var rec = JsonSerializer.Deserialize(File.ReadAllBytes(CloneJsonPath(dir)), InstancesJson.Default.CloneRecord);
            // Lists an older clone.json lacks (or has as null) come back as null, not empty.
            return rec is null ? null : rec with
            {
                RootFiles = rec.RootFiles ?? [],
                Copies = rec.Copies is null ? new(StringComparer.OrdinalIgnoreCase) : new(rec.Copies, StringComparer.OrdinalIgnoreCase),
            };
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    // Links (or copies) one root file of the base to target, which must not exist.
    private void PlaceRootFile(FileInfo source, string target)
    {
        if (CopiedFile(source.Name))
        {
            source.CopyTo(target);
            return;
        }
        int error = HardLink(target, source.FullName);
        if (error == 0)
            return;
        if (!FileLinks.IsNoLinkError(error))
            throw new IOException($"could not link {target} to {source.FullName}: {new System.ComponentModel.Win32Exception(error).Message}");
        if (!_copyWarned)
        {
            _copyWarned = true;
            Warn?.Invoke($"cannot hard-link from {source.DirectoryName} into {Path.GetDirectoryName(target)} " +
                "(another volume, or a file system without hard links): copying the game files instead (about 1 GB per clone); " +
                "set BEDD_INSTANCES_DIR to a folder on the volume of the LeviLauncher instance to link them");
        }
        source.CopyTo(target); // CopyFile keeps the time, so a refresh compares it like a link
    }

    // The clone's copies of version.json and .levilauncher\instance.json with enableIsolation on.
    private static void ForceIsolation(string baseDir, string dir)
    {
        foreach (var rel in new[] { "version.json", Path.Combine(".levilauncher", "instance.json") })
        {
            string from = Path.Combine(baseDir, rel);
            if (File.Exists(from))
                WriteMetadata(from, Path.Combine(dir, rel));
        }
    }

    private static void WriteMetadata(string from, string to)
    {
        JsonNode? node;
        try
        {
            node = JsonNode.Parse(File.ReadAllBytes(from));
        }
        catch (JsonException)
        {
            node = null;
        }
        if (node is JsonObject o)
        {
            o["enableIsolation"] = true;
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            FileLinks.ReplaceFile(to, JsonText.Indented(o));
        }
        else
        {
            File.Copy(from, to, overwrite: true);
        }
        File.SetLastWriteTimeUtc(to, File.GetLastWriteTimeUtc(from));
    }

    // What differs between a clone and its base (without changing anything): root files to link
    // again, metadata to copy again, junctions to rebuild, loader mods to replace.
    public static CloneDiff Compare(string baseDir, string dir)
    {
        var diff = new CloneDiff();
        var rec = ReadRecord(dir);
        var baseFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var f in BaseRootFiles(baseDir))
        {
            baseFiles.Add(f.Name);
            var t = new FileInfo(Path.Combine(dir, f.Name));
            if (IsMetadata(f.Name))
            {
                if (!t.Exists || t.LastWriteTimeUtc != f.LastWriteTimeUtc)
                    diff.Metadata.Add(f.Name);
                continue;
            }
            // A copy the game may have rewritten: stale when the base's file changed since the copy.
            if (t.Exists && CopiedFile(f.Name) && rec?.Copies.TryGetValue(f.Name, out var stamp) == true)
            {
                if (stamp != Stamp(f))
                    diff.RootFiles.Add(f.Name);
                continue;
            }
            if (!t.Exists || t.Length != f.Length || t.LastWriteTimeUtc != f.LastWriteTimeUtc)
                diff.RootFiles.Add(f.Name);
        }
        // Root files the clone got from the base that the base no longer has.
        foreach (var name in rec?.RootFiles ?? [])
            if (!baseFiles.Contains(name) && File.Exists(Path.Combine(dir, name)))
                diff.Removed.Add(name);
        // Folders the base got since.
        foreach (var d in BaseCopiedDirs(baseDir))
            if (!Directory.Exists(Path.Combine(dir, d.Name)))
                diff.NewDirs.Add(d.Name);
        string meta = Path.Combine(".levilauncher", "instance.json");
        var bm = new FileInfo(Path.Combine(baseDir, meta));
        var cm = new FileInfo(Path.Combine(dir, meta));
        if (bm.Exists && (!cm.Exists || cm.LastWriteTimeUtc != bm.LastWriteTimeUtc))
            diff.Metadata.Add(meta);
        foreach (var j in Junctions)
        {
            string from = Path.Combine(baseDir, j);
            if (!Directory.Exists(from))
                continue;
            string link = Path.Combine(dir, j);
            var target = FileLinks.LinkTarget(link);
            if (target is null || !Directory.Exists(target) ||
                !string.Equals(target.TrimEnd('\\'), Path.GetFullPath(from).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                diff.Junctions.Add(j);
        }
        foreach (var m in LoaderMods)
        {
            string from = Path.Combine(baseDir, "mods", m);
            if (!Directory.Exists(from))
                continue;
            string to = Path.Combine(dir, "mods", m);
            if (!Directory.Exists(to) || ModVersion(from) != ModVersion(to))
                diff.LoaderMods.Add(m);
        }
        return diff;
    }

    // Brings the clone at dir up to date with its base; returns what was changed.
    public CloneDiff Refresh(string baseDir, string dir)
    {
        var diff = Compare(baseDir, dir);
        foreach (var name in diff.RootFiles)
        {
            string target = Path.Combine(dir, name);
            if (File.Exists(target))
                FileLinks.DeleteFile(target); // only this name: a hard link's other names keep the file
            PlaceRootFile(new FileInfo(Path.Combine(baseDir, name)), target);
        }
        foreach (var name in diff.Removed)
            FileLinks.DeleteFile(Path.Combine(dir, name));
        foreach (var name in diff.NewDirs)
        {
            Progress?.Invoke($"copying {name}");
            FileLinks.CopyTree(Path.Combine(baseDir, name), Path.Combine(dir, name));
        }
        foreach (var rel in diff.Metadata)
            WriteMetadata(Path.Combine(baseDir, rel), Path.Combine(dir, rel));
        foreach (var j in diff.Junctions)
        {
            string link = Path.Combine(dir, j);
            var info = new DirectoryInfo(link);
            if (info.Exists && !FileLinks.IsLink(info))
            {
                Warn?.Invoke($"{link} is a real folder, not a junction to the base's; leaving it");
                continue;
            }
            if (info.Exists || File.Exists(link))
                FileLinks.RemoveLink(link);
            FileLinks.CreateJunction(link, Path.Combine(baseDir, j));
        }
        foreach (var m in diff.LoaderMods)
        {
            string to = Path.Combine(dir, "mods", m);
            Progress?.Invoke($"updating mods\\{m} from the base ({ModVersion(Path.Combine(baseDir, "mods", m)) ?? "?"})");
            if (Directory.Exists(to))
                FileLinks.DeleteTree(to);
            FileLinks.CopyTree(Path.Combine(baseDir, "mods", m), to);
        }
        foreach (var name in FreshDirs)
            Directory.CreateDirectory(Path.Combine(dir, name));
        // What the clone now holds of the base, and the game version it is now.
        var rec = ReadRecord(dir);
        if (rec is not null)
        {
            var now = rec with
            {
                BaseGameVersion = BaseGameVersion(baseDir) ?? rec.BaseGameVersion,
                RootFiles = BaseRootFiles(baseDir).Select(f => f.Name).ToList(),
                Copies = CopyStamps(baseDir),
            };
            if (!JsonText.Indented(now, InstancesJson.Default.CloneRecord).AsSpan()
                    .SequenceEqual(JsonText.Indented(rec, InstancesJson.Default.CloneRecord)))
                WriteRecord(dir, now);
        }
        return diff;
    }

    // "version" of a mod's manifest.json, or null.
    public static string? ModVersion(string modDir)
    {
        try
        {
            return (string?)JsonNode.Parse(File.ReadAllBytes(Path.Combine(modDir, "manifest.json")))?["version"];
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException or InvalidOperationException)
        {
            return null;
        }
    }
}

public sealed class CloneDiff
{
    public List<string> RootFiles { get; } = [];
    public List<string> Metadata { get; } = [];
    public List<string> Junctions { get; } = [];
    public List<string> LoaderMods { get; } = [];
    // Root files the base dropped, folders it added.
    public List<string> Removed { get; } = [];
    public List<string> NewDirs { get; } = [];
    public bool Empty => Count == 0;
    public int Count => RootFiles.Count + Metadata.Count + Junctions.Count + LoaderMods.Count + Removed.Count + NewDirs.Count;
    // Whether the clone is out of date (as opposed to a broken junction).
    public bool Stale => RootFiles.Count + Metadata.Count + LoaderMods.Count + Removed.Count + NewDirs.Count > 0;

    public JsonObject ToJson() => new()
    {
        ["rootFiles"] = new JsonArray(RootFiles.Select(x => (JsonNode?)x).ToArray()),
        ["metadata"] = new JsonArray(Metadata.Select(x => (JsonNode?)x).ToArray()),
        ["junctions"] = new JsonArray(Junctions.Select(x => (JsonNode?)x).ToArray()),
        ["loaderMods"] = new JsonArray(LoaderMods.Select(x => (JsonNode?)x).ToArray()),
        ["removed"] = new JsonArray(Removed.Select(x => (JsonNode?)x).ToArray()),
        ["newDirs"] = new JsonArray(NewDirs.Select(x => (JsonNode?)x).ToArray()),
    };
}
