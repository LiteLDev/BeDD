using System.Diagnostics;
using System.IO.Compression;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace BeDD.Core;

// lip, LeviLamina's package manager (github.com/LiteLDev/lip), which installs LeviLamina's server
// package (and with it the BDS) into a server folder. Found on the PC, or downloaded per user into
// Paths.Tools with the .NET runtime it runs on. Override: BEDD_LIP (lip.exe).
public static partial class Lip
{
    // lip is a .NET program of this major version.
    public const int DotnetMajor = 10;

    private static readonly HttpClient Http = CreateHttp();

    // The download progress lines lip prints ("12.3 / 45.6 MiB [...", or "Downloading <url>: 78%",
    // which lip wraps at the console width).
    [GeneratedRegex(@"( / [\d.]+ [KMG]iB \[|: \d{1,3}%$|^Downloading\s*$|^https?://\S*$)")]
    private static partial Regex ProgressLine();

    internal static bool IsProgressLine(string line) => ProgressLine().IsMatch(line);

    // lip.exe: BEDD_LIP, lip.exe on PATH, the copy in Paths.Tools, else a download of the latest lip
    // release into Paths.Tools\lip.
    public static string Find(Action<string> notice)
    {
        var env = Environment.GetEnvironmentVariable("BEDD_LIP");
        if (!string.IsNullOrWhiteSpace(env))
        {
            if (!File.Exists(env))
                throw new FileNotFoundException($"BEDD_LIP points at {env}, which does not exist");
            return env;
        }
        var onPath = FindOnPath("lip.exe");
        if (onPath is not null)
            return onPath;
        var dir = Path.Combine(Paths.Tools, "lip");
        var local = FindIn(dir);
        if (local is not null)
            return local;
        var zip = Path.Combine(Paths.Tools, "lip.zip");
        GitHubAsset("LiteLDev/lip", n => n.StartsWith("lip-", StringComparison.OrdinalIgnoreCase) &&
            n.EndsWith("-win-x64.zip", StringComparison.OrdinalIgnoreCase), zip, null, notice);
        ZipFile.ExtractToDirectory(zip, dir, overwriteFiles: true);
        File.Delete(zip);
        return FindIn(dir) ?? throw new FileNotFoundException($"the lip download has no lip.exe ({dir})");
    }

    private static string? FindIn(string dir)
    {
        if (!Directory.Exists(dir))
            return null;
        var direct = Path.Combine(dir, "lip.exe");
        if (File.Exists(direct))
            return direct;
        return Directory.EnumerateFiles(dir, "lip.exe", SearchOption.AllDirectories).FirstOrDefault();
    }

    internal static string? FindOnPath(string exe, string? pathVar = null)
    {
        pathVar ??= Environment.GetEnvironmentVariable("PATH") ?? "";
        foreach (var raw in pathVar.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            try
            {
                var p = Path.Combine(raw.Trim('"'), exe);
                if (File.Exists(p))
                    return p;
            }
            catch (ArgumentException)
            {
            }
        }
        return null;
    }

    // Whether root (a dotnet folder) has a Microsoft.NETCore.App runtime of DotnetMajor.
    internal static bool HasRuntime(string? root)
    {
        if (string.IsNullOrWhiteSpace(root))
            return false;
        var shared = Path.Combine(root, "shared", "Microsoft.NETCore.App");
        if (!Directory.Exists(shared))
            return false;
        return Directory.EnumerateDirectories(shared, DotnetMajor + ".*").Any();
    }

    // The DOTNET_ROOT lip must run with: null when DOTNET_ROOT already names a runtime of the right
    // version, the machine-wide dotnet folder when that has one (DOTNET_ROOT, when set, would win
    // over it), else the per-user one in %LOCALAPPDATA%\Microsoft\dotnet (where LeviLauncher
    // installs it too), downloaded first when missing.
    public static string? EnsureDotnetRuntime(Action<string> notice)
    {
        var env = Environment.GetEnvironmentVariable("DOTNET_ROOT");
        if (HasRuntime(env))
            return null;
        var machine = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "dotnet");
        if (HasRuntime(machine))
            return string.IsNullOrWhiteSpace(env) ? null : machine;
        var user = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Microsoft", "dotnet");
        if (HasRuntime(user))
            return user;

        notice($"installing the .NET {DotnetMajor} runtime for lip (per user, into {user})");
        string versionText = Get($"https://builds.dotnet.microsoft.com/dotnet/Runtime/{DotnetMajor}.0/latest.version");
        string version = LatestVersion(versionText) ??
            throw new IOException($"no .NET {DotnetMajor} runtime version in the download index");
        var staging = Path.Combine(user, ".bedd-download");
        if (Directory.Exists(staging))
            Directory.Delete(staging, recursive: true);
        Directory.CreateDirectory(staging);
        var zip = Path.Combine(staging, "runtime.zip");
        Download($"https://builds.dotnet.microsoft.com/dotnet/Runtime/{version}/dotnet-runtime-{version}-win-x64.zip", zip);
        var extracted = Path.Combine(staging, "x");
        ZipFile.ExtractToDirectory(zip, extracted);
        // The files around the runtime (dotnet.exe, the host) only where another runtime has not put
        // them already; the runtime folder itself last and in one move, so an interrupted install
        // never leaves a runtime HasRuntime takes for complete.
        var runtimeRel = Path.Combine("shared", "Microsoft.NETCore.App", version);
        foreach (var f in Directory.EnumerateFiles(extracted, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(extracted, f);
            if (rel.StartsWith(runtimeRel + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                continue;
            var target = Path.Combine(user, rel);
            if (File.Exists(target))
                continue;
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Move(f, target);
        }
        var runtimeTarget = Path.Combine(user, runtimeRel);
        if (Directory.Exists(runtimeTarget))
            Directory.Delete(runtimeTarget, recursive: true);
        Directory.CreateDirectory(Path.GetDirectoryName(runtimeTarget)!);
        Directory.Move(Path.Combine(extracted, runtimeRel), runtimeTarget);
        Directory.Delete(staging, recursive: true);
        if (!HasRuntime(user))
            throw new IOException($"the .NET {DotnetMajor} runtime for lip did not install into {user}");
        return user;
    }

    // The version in a latest.version file: its last non-empty line (older files put a commit hash
    // first).
    internal static string? LatestVersion(string text)
    {
        var lines = text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        var v = lines.Length > 0 ? lines[^1] : null;
        return v is not null && Regex.IsMatch(v, @"^\d+\.\d+\.\d+[\w.-]*$") ? v : null;
    }

    // Runs lip with args in workDir and returns its exit code. Each line it prints (without the
    // download progress) goes to output.
    public static int Run(string lip, IEnumerable<string> args, string workDir, string? dotnetRoot, Action<string> output)
    {
        var psi = new ProcessStartInfo(lip)
        {
            WorkingDirectory = workDir,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var a in args)
            psi.ArgumentList.Add(a);
        if (dotnetRoot is not null)
            psi.Environment["DOTNET_ROOT"] = dotnetRoot;
        using var p = new Process { StartInfo = psi };
        object gate = new();
        void Line(string? l)
        {
            if (l is null || IsProgressLine(l))
                return;
            lock (gate)
                output(l);
        }
        p.OutputDataReceived += (_, e) => Line(e.Data);
        p.ErrorDataReceived += (_, e) => Line(e.Data);
        p.Start();
        p.StandardInput.Close(); // lip never waits for an answer
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        p.WaitForExit();
        return p.ExitCode;
    }

    // Downloads the first asset of a GitHub release (tag, or the latest one) whose name matches to
    // outFile and returns outFile.
    public static string GitHubAsset(string repo, Func<string, bool> match, string outFile, string? tag, Action<string> notice)
    {
        string api = tag is null
            ? $"https://api.github.com/repos/{repo}/releases/latest"
            : $"https://api.github.com/repos/{repo}/releases/tags/{tag}";
        var release = JsonNode.Parse(Get(api)) ?? throw new IOException($"no release of {repo}");
        string releaseTag = (string?)release["tag_name"] ?? "?";
        JsonNode? asset = null;
        foreach (var a in release["assets"]?.AsArray() ?? [])
        {
            if (a?["name"] is JsonValue n && n.TryGetValue(out string? name) && match(name))
            {
                asset = a;
                break;
            }
        }
        if (asset is null)
            throw new IOException($"no matching asset in {repo} {releaseTag}");
        string url = (string?)asset["browser_download_url"] ?? throw new IOException($"asset of {repo} {releaseTag} has no url");
        notice($"downloading {(string?)asset["name"]} ({repo} {releaseTag})");
        Download(url, outFile);
        return outFile;
    }

    private static string Get(string url) => Http.GetStringAsync(url).GetAwaiter().GetResult();

    private static void Download(string url, string outFile)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outFile))!);
        var tmp = outFile + ".download";
        using (var response = Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult())
        {
            response.EnsureSuccessStatusCode();
            using var src = response.Content.ReadAsStream();
            using var dst = File.Create(tmp);
            src.CopyTo(dst);
        }
        File.Move(tmp, outFile, overwrite: true);
    }

    private static HttpClient CreateHttp()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("BeDD/" + BuildInfo.Version);
        return c;
    }
}
