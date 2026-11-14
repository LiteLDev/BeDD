using System.Text.Json;
using System.Text.Json.Nodes;
using BeDD.Cli;

namespace BeDD.Core;

// What mod install/config change in mods\BeDD\config\config.json. Only what is given is written:
// every other setting keeps the mod's default.
public sealed class ModSettings
{
    // "key=value" (a dotted key reaches into objects); the value is JSON when it parses as JSON,
    // else a string.
    public IReadOnlyList<string> Set { get; init; } = [];
    public IReadOnlyList<string> Unset { get; init; } = [];
    // ClientOnly, ClientServer or Remote (config "mode").
    public string? WorldMode { get; init; }
    // The server a Remote game joins: a host off this PC goes into "allowHosts".
    public string? ServerHost { get; init; }

    public bool Empty => Set.Count == 0 && Unset.Count == 0 && WorldMode is null && ServerHost is null;
}

// Puts the BeDD mod into an instance (mods\BeDD) and takes it out again.
public static class ModDeploy
{
    public const string ModName = "BeDD";
    public static readonly string[] WorldModes = ["ClientOnly", "ClientServer", "Remote"];

    public static string ModDir(InstanceInfo i) => Path.Combine(i.ModsDir, ModName);
    public static string ConfigPath(InstanceInfo i) => Path.Combine(ModDir(i), "config", "config.json");

    // Where the mod's files come from: from, else BEDD_MOD_DIR, else mod\BeDD next to BeDD.exe (a
    // release), else the nearest bin\BeDD up from the current folder (a development checkout).
    public static string FindSource(string? from, string? cwd = null, string? appDir = null)
    {
        if (!string.IsNullOrWhiteSpace(from))
            return Checked(Path.GetFullPath(from), "--from");
        var env = Environment.GetEnvironmentVariable("BEDD_MOD_DIR");
        if (!string.IsNullOrWhiteSpace(env))
            return Checked(Path.GetFullPath(env.Trim()), "BEDD_MOD_DIR");
        string release = Path.Combine(appDir ?? AppContext.BaseDirectory, "mod", ModName);
        if (File.Exists(Path.Combine(release, "BeDD.dll")))
            return release;
        for (var d = new DirectoryInfo(cwd ?? Environment.CurrentDirectory); d is not null; d = d.Parent)
        {
            string dev = Path.Combine(d.FullName, "bin", ModName);
            if (File.Exists(Path.Combine(dev, "BeDD.dll")))
                return dev;
        }
        throw CliError.NotFound("ERR_MOD_SOURCE", "found no build of the BeDD mod (mod\\BeDD next to BeDD.exe, or bin\\BeDD of a checkout)",
            $"{Dispatcher.ProgramName} mod install <instance> --from <folder with BeDD.dll>");
    }

    private static string Checked(string dir, string what)
    {
        if (!File.Exists(Path.Combine(dir, "BeDD.dll")) || !File.Exists(Path.Combine(dir, "manifest.json")))
            throw CliError.NotFound("ERR_MOD_SOURCE", $"{dir} ({what}) has no BeDD.dll and manifest.json");
        return dir;
    }

    // The file in mods\BeDD that names the folder a "mod install --from" took the mod from. While it
    // exists, launch keeps the mod current with that folder, not with the default source.
    public const string PinFile = "source.pin";

    // Copies the mod into the instance, writes its config (only the given settings) and switches
    // LeviLamina to its built-in crash logger. Replaces an earlier install, config included. pin:
    // the source was named explicitly (--from), so launch keeps following it (PinFile).
    public static JsonObject Install(InstanceInfo i, string source, ModSettings settings, Action<string>? warn = null, bool pin = false)
    {
        var config = new JsonObject();
        Apply(config, settings);
        using var _ = SetupLock.Acquire();
        RefuseRunning(i);
        string dest = ModDir(i);
        if (Directory.Exists(dest))
            FileLinks.DeleteTree(dest);
        Directory.CreateDirectory(Path.Combine(dest, "config"));
        var files = new JsonArray();
        foreach (var f in ModFiles)
        {
            string from = Path.Combine(source, f);
            if (!File.Exists(from))
                continue;
            File.Copy(from, Path.Combine(dest, f));
            files.Add((JsonNode)f);
        }
        if (pin)
            File.WriteAllText(Path.Combine(dest, PinFile), Path.GetFullPath(source));
        WriteConfig(i, config);
        string crash = SwitchCrashLogger(i.ModsDir, warn);
        return new JsonObject
        {
            ["instance"] = i.Name,
            ["dir"] = dest,
            ["source"] = source,
            ["version"] = Cloner.ModVersion(dest),
            ["pinned"] = pin,
            ["files"] = files,
            ["config"] = config.DeepClone(),
            ["crashLogger"] = crash,
        };
    }

    // The files of the mod that a build is made of; the pdb is optional.
    private static readonly string[] ModFiles = ["BeDD.dll", "BeDD.pdb", "manifest.json"];

    // What EnsureCurrent did: "installed", "updated", "current", "running" (left alone, its game
    // runs), "no-source" (no build of the mod to compare with) or "pinned-missing" (the folder of
    // the PinFile has no build any more: left alone; From is that folder).
    public sealed record EnsureResult(string Action, string? From, string? To);

    // Keeps the mod of a BeDD clone the build that comes with this BeDD.exe, so a clone made before
    // BeDD was upgraded does not run the old mod: installs it when missing (with the mod's default
    // config), and when the build differs, copies its files over and keeps the clone's config. The
    // source is found as by FindSource(null); without one, nothing changes.
    public static EnsureResult EnsureCurrent(InstanceInfo i, Action<string>? warn = null, string? cwd = null, string? appDir = null)
    {
        string dest = ModDir(i);
        string source;
        string pinFile = Path.Combine(dest, PinFile);
        if (File.Exists(pinFile))
        {
            source = File.ReadAllText(pinFile).Trim();
            if (!File.Exists(Path.Combine(source, "BeDD.dll")) || !File.Exists(Path.Combine(source, "manifest.json")))
                return new EnsureResult("pinned-missing", source, null);
        }
        else
        {
            try
            {
                source = FindSource(null, cwd, appDir);
            }
            catch (CliError)
            {
                return new EnsureResult("no-source", null, null);
            }
        }
        string? to = Cloner.ModVersion(source);
        if (!File.Exists(Path.Combine(dest, "BeDD.dll")))
        {
            if (FileLinks.ProcessesIn(i.Dir).Count > 0)
                return new EnsureResult("running", null, to);
            Install(i, source, new ModSettings(), warn);
            return new EnsureResult("installed", null, to);
        }
        if (SameBuild(source, dest))
            return new EnsureResult("current", to, to);
        string? from = Cloner.ModVersion(dest);
        using var _ = SetupLock.Acquire();
        if (FileLinks.ProcessesIn(i.Dir).Count > 0)
            return new EnsureResult("running", from, to);
        foreach (var f in ModFiles)
        {
            string src = Path.Combine(source, f), dst = Path.Combine(dest, f);
            if (File.Exists(src))
                File.Copy(src, dst, overwrite: true);
            else if (File.Exists(dst))
                File.Delete(dst);
        }
        SwitchCrashLogger(i.ModsDir, warn);
        return new EnsureResult("updated", from, to);
    }

    // The same files with the same bytes (the pdb is not compared: it is no part of what runs).
    internal static bool SameBuild(string a, string b)
    {
        foreach (var f in new[] { "BeDD.dll", "manifest.json" })
        {
            var fa = new FileInfo(Path.Combine(a, f));
            var fb = new FileInfo(Path.Combine(b, f));
            if (!fa.Exists || !fb.Exists || fa.Length != fb.Length)
                return false;
            if (!File.ReadAllBytes(fa.FullName).AsSpan().SequenceEqual(File.ReadAllBytes(fb.FullName)))
                return false;
        }
        return true;
    }

    // Removes mods\BeDD; false when it was not there.
    public static bool Uninstall(InstanceInfo i)
    {
        using var _ = SetupLock.Acquire();
        RefuseRunning(i);
        string dir = ModDir(i);
        if (!Directory.Exists(dir))
            return false;
        FileLinks.DeleteTree(dir);
        return true;
    }

    // The mod's config after applying settings (written only when they change something).
    public static JsonObject Configure(InstanceInfo i, ModSettings settings)
    {
        if (!File.Exists(Path.Combine(ModDir(i), "BeDD.dll")))
            throw CliError.NotFound("ERR_MOD_NOT_INSTALLED", $"{i.Name} has no BeDD mod", $"{Dispatcher.ProgramName} mod install {i.Name}");
        if (settings.Empty)
            return ReadConfig(i);
        using var _ = SetupLock.Acquire();
        var config = ReadConfig(i);
        Apply(config, settings);
        WriteConfig(i, config);
        return config;
    }

    public static JsonObject ReadConfig(InstanceInfo i)
    {
        string path = ConfigPath(i);
        if (!File.Exists(path))
            return new JsonObject();
        try
        {
            return JsonNode.Parse(File.ReadAllBytes(path), documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true }) as JsonObject
                ?? throw CliError.Failure("ERR_CONFIG", $"{path} is not a JSON object");
        }
        catch (JsonException e)
        {
            throw CliError.Failure("ERR_CONFIG", $"{path} is not valid JSON: {e.Message}");
        }
    }

    private static void WriteConfig(InstanceInfo i, JsonObject config)
    {
        string path = ConfigPath(i);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        FileLinks.ReplaceFile(path, JsonText.Indented(config));
    }

    private static void RefuseRunning(InstanceInfo i) => InstanceStore.ThrowIfRunning(i.Name, i.Dir);

    public static void Apply(JsonObject config, ModSettings s)
    {
        foreach (var kv in s.Set)
            ApplySet(config, kv);
        foreach (var key in s.Unset)
            ApplyUnset(config, key);
        if (s.WorldMode is not null || s.ServerHost is not null)
            ApplyWorldMode(config, s.WorldMode, s.ServerHost);
    }

    public static void ApplySet(JsonObject config, string kv)
    {
        int eq = kv.IndexOf('=');
        if (eq <= 0)
            throw CliError.Usage($"--set takes key=value, not '{kv}'");
        var (parent, last) = Walk(config, kv[..eq].Trim(), create: true);
        parent![last] = ParseValue(kv[(eq + 1)..]);
    }

    public static void ApplyUnset(JsonObject config, string key)
    {
        var (parent, last) = Walk(config, key.Trim(), create: false);
        parent?.Remove(last);
    }

    // A JSON value when text parses as one (true, 3, 0.5, [..], {..}, "quoted", null), else the text.
    public static JsonNode? ParseValue(string text)
    {
        try
        {
            return JsonNode.Parse(text);
        }
        catch (JsonException)
        {
            return JsonValue.Create(text);
        }
    }

    private static (JsonObject? Parent, string Last) Walk(JsonObject root, string key, bool create)
    {
        if (key.Length == 0)
            throw CliError.Usage("a config key is empty");
        var parts = key.Split('.');
        if (parts.Any(p => p.Length == 0))
            throw CliError.Usage($"'{key}' is not a config key");
        JsonObject o = root;
        for (int n = 0; n < parts.Length - 1; n++)
        {
            if (o[parts[n]] is JsonObject child)
            {
                o = child;
                continue;
            }
            if (!create)
                return (null, parts[^1]);
            if (o.ContainsKey(parts[n]) && o[parts[n]] is not null)
                throw CliError.Usage($"config '{string.Join('.', parts.Take(n + 1))}' is not an object");
            var made = new JsonObject();
            o[parts[n]] = made;
            o = made;
        }
        return (o, parts[^1]);
    }

    // Config "mode" and "allowHosts" by the rules of the mod: Remote needs the server's host, and a
    // host off this PC goes into allowHosts (the one host loopbackOnly then lets the game reach).
    public static void ApplyWorldMode(JsonObject config, string? mode, string? host)
    {
        host = string.IsNullOrWhiteSpace(host) ? null : host.Trim();
        if (mode is null)
            throw CliError.Usage("--server-host goes with --world-mode Remote");
        string canonical = WorldModes.FirstOrDefault(m => string.Equals(m, mode, StringComparison.OrdinalIgnoreCase))
            ?? throw CliError.Usage($"--world-mode is ClientOnly, ClientServer or Remote, not '{mode}'");
        if (canonical == "Remote" && host is null)
            throw CliError.Usage("--world-mode Remote needs --server-host (the server the game joins)");
        if (canonical != "Remote" && host is not null)
            throw CliError.Usage($"--server-host is for --world-mode Remote, not {canonical}");
        config["mode"] = canonical;
        config.Remove("allowHosts");
        if (canonical == "Remote" && !OnThisPc(host!))
            config["allowHosts"] = new JsonArray(host);
    }

    public static bool OnThisPc(string host) =>
        host.Equals("localhost", StringComparison.OrdinalIgnoreCase) || host.StartsWith("127.", StringComparison.Ordinal) ||
        host is "::1" or "[::1]";

    // LeviLamina starts CrashLogger.exe, a console program, as the game loads: the game has no
    // console, so Windows opens one that flashes on the desktop at every start. The built-in crash
    // logger catches crashes inside the game process instead (logs\crash as before). Returns
    // "switched", "builtin" (already), "disabled" (crash logger off), "noConfig" (LeviLamina writes
    // its config at the game's first start) or "error".
    public static string SwitchCrashLogger(string modsDir, Action<string>? warn = null)
    {
        string file = Path.Combine(modsDir, "LeviLamina", "config", "Config.json");
        if (!File.Exists(file))
            return "noConfig";
        try
        {
            var root = JsonNode.Parse(File.ReadAllBytes(file), documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
            if (root?["modules"]?["crashLogger"] is not JsonObject cl)
                return "disabled";
            if (cl["enabled"] is JsonValue en && en.TryGetValue(out bool enabled) && !enabled)
                return "disabled";
            if (cl["builtin"] is JsonValue bi && bi.TryGetValue(out bool builtin) && builtin)
                return "builtin";
            cl["builtin"] = true;
            FileLinks.ReplaceFile(file, JsonText.Indented(root));
            return "switched";
        }
        catch (Exception e) when (e is JsonException or IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            warn?.Invoke($"could not switch LeviLamina to its built-in crash logger ({file}): {e.Message}");
            return "error";
        }
    }
}
