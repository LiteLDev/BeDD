using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using BeDD.Cli;
using Microsoft.Win32;
using Registry = Microsoft.Win32.Registry;

namespace BeDD.Core;

// One entry of "LeviLauncher.exe instance list --json" (LeviLauncher v1.1+).
public sealed record LeviLauncherInstance
{
    public string Name { get; init; } = "";
    public string GameVersion { get; init; } = "";
    // "release" or "preview".
    public string Channel { get; init; } = "";
    // "gdk" or "uwp".
    public string Type { get; init; } = "";
    public bool Isolated { get; init; }
    public bool Registered { get; init; }
    public bool Running { get; init; }
    public int[] Pids { get; init; } = [];
    // LeviLamina's version in the instance, null without it.
    public string? LoaderVersion { get; init; }
    public string Path { get; init; } = "";
    public bool IsDefault { get; init; }
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull, NumberHandling = JsonNumberHandling.AllowReadingFromString)]
[JsonSerializable(typeof(List<LeviLauncherInstance>))]
internal sealed partial class LeviLauncherJson : JsonSerializerContext;

// What BeDD needs from LeviLauncher: the instances it owns. Tests fake it.
public interface ILeviLauncher
{
    string Exe { get; }
    IReadOnlyList<LeviLauncherInstance> ListInstances();
}

// LeviLauncher.exe's command line. BeDD only reads through it: it never writes into LeviLauncher's
// instances (they are the user's), it clones them.
public sealed class LeviLauncherCli : ILeviLauncher
{
    public const string ReleasesUrl = "https://github.com/LiteLDev/LeviLauncher/releases/latest";
    public static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(60);

    private LeviLauncherCli(string exe, string version)
    {
        Exe = exe;
        Version = version;
    }

    public string Exe { get; }
    // What "version --json" reports.
    public string Version { get; }

    // The exe written so that a hint can be pasted into a shell as it is.
    public string Command => Exe.Contains(' ') ? $"\"{Exe}\"" : Exe;

    // The first LeviLauncher.exe with a command line: BEDD_LEVILAUNCHER (only that one when set),
    // else on PATH, else installed, else the newest under BeDD's tools folder.
    public static LeviLauncherCli Find(Action<string>? verbose = null)
    {
        var env = Environment.GetEnvironmentVariable("BEDD_LEVILAUNCHER");
        if (!string.IsNullOrWhiteSpace(env))
        {
            string exe = System.IO.Path.GetFullPath(env.Trim());
            if (!File.Exists(exe))
                throw CliError.NotFound("ERR_LEVILAUNCHER_NOT_FOUND", $"BEDD_LEVILAUNCHER points to {exe}, which does not exist",
                    "set BEDD_LEVILAUNCHER to the path of LeviLauncher.exe, or unset it");
            return Probe(exe, verbose) ?? throw NoCli(exe);
        }
        string? withoutCli = null;
        foreach (var exe in Candidates())
        {
            verbose?.Invoke($"trying {exe}");
            var cli = Probe(exe, verbose);
            if (cli is not null)
                return cli;
            withoutCli ??= exe;
        }
        if (withoutCli is not null)
            throw NoCli(withoutCli);
        throw CliError.NotFound("ERR_LEVILAUNCHER_NOT_FOUND",
            "LeviLauncher is not installed (BeDD clones the game from a LeviLauncher instance)",
            $"install LeviLauncher v1.1 or newer from {ReleasesUrl}, or set BEDD_LEVILAUNCHER to its LeviLauncher.exe");
    }

    private static CliError NoCli(string exe) => CliError.Failure("ERR_LEVILAUNCHER_TOO_OLD",
        $"{exe} has no command line (LeviLauncher v1.1 or newer is needed)",
        $"install LeviLauncher v1.1 or newer from {ReleasesUrl}");

    private static LeviLauncherCli? Probe(string exe, Action<string>? verbose)
    {
        try
        {
            var (code, stdout, stderr) = Run(exe, ["version", "--json"], TimeSpan.FromSeconds(30));
            if (code != 0)
            {
                verbose?.Invoke($"{exe} version --json: exit code {code} {stderr.Trim()}");
                return null;
            }
            var version = (string?)JsonNode.Parse(stdout)?["version"];
            if (version is null)
                return null;
            verbose?.Invoke($"LeviLauncher {version} at {exe}");
            return new LeviLauncherCli(exe, version);
        }
        catch (Exception e) when (e is JsonException or System.ComponentModel.Win32Exception or CliError or IOException)
        {
            verbose?.Invoke($"{exe} version --json: {e.Message}");
            return null;
        }
    }

    internal static IEnumerable<string> Candidates()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var c in RawCandidates())
        {
            string full;
            try
            {
                full = System.IO.Path.GetFullPath(c);
            }
            catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException)
            {
                continue;
            }
            if (File.Exists(full) && seen.Add(full))
                yield return full;
        }
    }

    private static IEnumerable<string> RawCandidates()
    {
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            yield return System.IO.Path.Combine(dir.Trim('"'), "LeviLauncher.exe");
        foreach (var dir in InstallLocations())
            yield return System.IO.Path.Combine(dir, "LeviLauncher.exe");
        yield return System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "LeviMC", "LeviLauncher", "LeviLauncher.exe");
        yield return System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "LeviLauncher", "LeviLauncher.exe");
        foreach (var exe in ToolsCandidates(Paths.Tools))
            yield return exe;
    }

    // InstallLocation of every uninstall entry whose DisplayName starts with LeviLauncher.
    private static IEnumerable<string> InstallLocations()
    {
        var found = new List<string>();
        (RegistryKey Hive, string Path)[] keys =
        [
            (Registry.CurrentUser, @"Software\Microsoft\Windows\CurrentVersion\Uninstall"),
            (Registry.LocalMachine, @"Software\Microsoft\Windows\CurrentVersion\Uninstall"),
            (Registry.LocalMachine, @"Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
        ];
        foreach (var (hive, path) in keys)
        {
            try
            {
                using var root = hive.OpenSubKey(path);
                if (root is null)
                    continue;
                foreach (var name in root.GetSubKeyNames())
                {
                    using var k = root.OpenSubKey(name);
                    if (k?.GetValue("DisplayName") is string display && display.StartsWith("LeviLauncher", StringComparison.OrdinalIgnoreCase) &&
                        k.GetValue("InstallLocation") is string loc && loc.Trim().Length > 0)
                        found.Add(loc.Trim().Trim('"'));
                }
            }
            catch (Exception e) when (e is System.Security.SecurityException or IOException or UnauthorizedAccessException)
            {
            }
        }
        return found;
    }

    // tools\LeviLauncher*\LeviLauncher.exe, the highest version in the folder name first (a folder
    // without a version counts as the oldest), then the newest file.
    internal static IEnumerable<string> ToolsCandidates(string toolsDir)
    {
        if (!Directory.Exists(toolsDir))
            return [];
        return Directory.EnumerateDirectories(toolsDir, "LeviLauncher*")
            .Select(d => (Dir: d, Exe: System.IO.Path.Combine(d, "LeviLauncher.exe")))
            .Where(x => File.Exists(x.Exe))
            .OrderByDescending(x => FolderVersion(System.IO.Path.GetFileName(x.Dir)))
            .ThenByDescending(x => File.GetLastWriteTimeUtc(x.Exe))
            .Select(x => x.Exe)
            .ToList();
    }

    private static Version FolderVersion(string name)
    {
        int dash = name.IndexOf('-');
        if (dash >= 0 && System.Version.TryParse(name[(dash + 1)..].TrimStart('v', 'V'), out var v))
            return v;
        return new Version(0, 0);
    }

    public IReadOnlyList<LeviLauncherInstance> ListInstances()
    {
        var (code, stdout, stderr) = Run(Exe, ["instance", "list", "--json", "--no-input"], DefaultTimeout);
        if (code != 0)
            throw CliError.Failure("ERR_LEVILAUNCHER", $"LeviLauncher.exe instance list failed (exit code {code}): {FirstLine(stderr, stdout)}",
                $"{Command} instance list");
        return ParseInstances(stdout);
    }

    internal static List<LeviLauncherInstance> ParseInstances(string json)
    {
        try
        {
            return JsonSerializer.Deserialize(json, LeviLauncherJson.Default.ListLeviLauncherInstance) ?? [];
        }
        catch (JsonException e)
        {
            throw CliError.Failure("ERR_LEVILAUNCHER", $"LeviLauncher.exe instance list printed no instance list ({e.Message})");
        }
    }

    private static string FirstLine(params string[] texts)
    {
        foreach (var t in texts)
        {
            var line = t.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).FirstOrDefault();
            if (line is not null)
                return line;
        }
        return "no output";
    }

    // Runs LeviLauncher.exe without a window and returns its exit code and UTF-8 output; kills it
    // after timeout.
    public static (int Code, string Stdout, string Stderr) Run(string exe, IReadOnlyList<string> args, TimeSpan timeout)
    {
        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = System.IO.Path.GetDirectoryName(exe) ?? Environment.CurrentDirectory,
        };
        foreach (var a in args)
            psi.ArgumentList.Add(a);
        psi.Environment["NO_COLOR"] = "1";
        using var p = Process.Start(psi) ?? throw CliError.Failure("ERR_LEVILAUNCHER", $"could not start {exe}");
        p.StandardInput.Close();
        var stdout = p.StandardOutput.ReadToEndAsync();
        var stderr = p.StandardError.ReadToEndAsync();
        if (!p.WaitForExit(timeout))
        {
            try
            {
                p.Kill(entireProcessTree: true);
            }
            catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception)
            {
            }
            throw CliError.Failure("ERR_LEVILAUNCHER_TIMEOUT",
                $"LeviLauncher.exe {string.Join(' ', args)} did not finish within {timeout.TotalSeconds.ToString(CultureInfo.InvariantCulture)} s");
        }
        p.WaitForExit();
        return (p.ExitCode, stdout.Result, stderr.Result);
    }
}

// Which LeviLauncher instance BeDD clones its games from.
public static class BaseSelection
{
    // "26.60." for LeviLamina 26.60.x: the loader line made for BuildInfo.GameVersion.
    public static string LoaderLine
    {
        get
        {
            var parts = BuildInfo.LeviLaminaVersion.Split('.');
            return parts.Length >= 2 ? $"{parts[0]}.{parts[1]}." : BuildInfo.LeviLaminaVersion;
        }
    }

    public static bool MatchesGame(string gameVersion) =>
        gameVersion == BuildInfo.GameVersion || gameVersion.StartsWith(BuildInfo.GameVersion + ".", StringComparison.Ordinal);

    // Why the instance cannot be a base, or null when it can.
    public static string? Unusable(LeviLauncherInstance i)
    {
        if (!MatchesGame(i.GameVersion))
            return $"game {i.GameVersion}, not {BuildInfo.GameVersion}";
        if (!string.Equals(i.Type, "gdk", StringComparison.OrdinalIgnoreCase))
            return $"{i.Type} build, not gdk";
        if (!string.Equals(i.Channel, "release", StringComparison.OrdinalIgnoreCase))
            return $"{i.Channel} channel, not release";
        if (string.IsNullOrWhiteSpace(i.LoaderVersion))
            return "no LeviLamina";
        if (!i.LoaderVersion.StartsWith(LoaderLine, StringComparison.Ordinal))
            return $"LeviLamina {i.LoaderVersion}, not {LoaderLine}x";
        if (string.IsNullOrWhiteSpace(i.Path) || !Directory.Exists(i.Path))
            return "its folder does not exist";
        return null;
    }

    // True when the instance's LeviLamina is older than the one BeDD is built against.
    public static bool LoaderOlder(LeviLauncherInstance i) =>
        System.Version.TryParse(i.LoaderVersion, out var have) && System.Version.TryParse(BuildInfo.LeviLaminaVersion, out var want) && have < want;

    // requested (a LeviLauncher instance name, else BEDD_INSTANCE), else the default instance when
    // usable, else the first usable one. launcherCommand is how hints start LeviLauncher.exe.
    public static LeviLauncherInstance Select(IReadOnlyList<LeviLauncherInstance> all, string? requested, string launcherCommand = "LeviLauncher.exe",
        bool useEnvironment = true)
    {
        if (string.IsNullOrWhiteSpace(requested) && useEnvironment)
            requested = Environment.GetEnvironmentVariable("BEDD_INSTANCE");
        if (!string.IsNullOrWhiteSpace(requested))
        {
            requested = requested.Trim();
            var named = all.FirstOrDefault(i => string.Equals(i.Name, requested, StringComparison.OrdinalIgnoreCase))
                ?? throw CliError.NotFound("ERR_BASE_NOT_FOUND", $"LeviLauncher has no instance '{requested}'", $"{launcherCommand} instance list");
            var why = Unusable(named);
            if (why is not null)
                throw Rejected(named, why, launcherCommand);
            return named;
        }
        var def = all.FirstOrDefault(i => i.IsDefault);
        if (def is not null && Unusable(def) is null)
            return def;
        var first = all.FirstOrDefault(i => Unusable(i) is null);
        if (first is not null)
            return first;

        // Nothing usable: say what is closest to usable.
        var sameGame = all.Where(i => MatchesGame(i.GameVersion) && string.Equals(i.Type, "gdk", StringComparison.OrdinalIgnoreCase) &&
            string.Equals(i.Channel, "release", StringComparison.OrdinalIgnoreCase)).ToList();
        if (sameGame.Count > 0)
        {
            var near = sameGame.FirstOrDefault(i => i.IsDefault) ?? sameGame[0];
            throw Rejected(near, Unusable(near)!, launcherCommand);
        }
        throw CliError.NotFound("ERR_NO_BASE",
            $"LeviLauncher has no Minecraft {BuildInfo.GameVersion} (GDK, release) instance with LeviLamina",
            $"{launcherCommand} account login && {launcherCommand} install {BuildInfo.GameVersion} --loader");
    }

    private static CliError Rejected(LeviLauncherInstance i, string why, string launcherCommand)
    {
        string? hint = why.StartsWith("no LeviLamina", StringComparison.Ordinal) || why.StartsWith("LeviLamina ", StringComparison.Ordinal)
            ? $"{launcherCommand} loader install \"{i.Name}\""
            : $"{launcherCommand} install {BuildInfo.GameVersion} --loader";
        return CliError.Failure("ERR_BASE_UNUSABLE", $"LeviLauncher instance '{i.Name}' cannot be cloned: {why}", hint);
    }
}
