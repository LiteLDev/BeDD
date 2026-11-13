using System.Diagnostics;
using System.Net.NetworkInformation;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.System.Threading;

namespace BeDD.Core;

// A Bedrock Dedicated Server of BeDD's (Paths.Servers\<Name>) and what it still lacks.
public sealed record ServerInfo
{
    public required string Name { get; init; }
    public required string Dir { get; init; }
    // The BDS exe with LeviLamina's loader (LeviLamina's install makes it from bedrock_server.exe).
    public string Exe => Path.Combine(Dir, "bedrock_server_mod.exe");
    public string ModsDir => Path.Combine(Dir, "mods");
    public string PropertiesFile => Path.Combine(Dir, "server.properties");
    // Everything the server prints, written by its console host.
    public string LogPath => Path.Combine(Dir, "logs", "console.log");
    public int Port { get; init; }
    public int PortV6 { get; init; }
    // The BeDD module (mods\<its name>), its platform, whether LeviLamina loads it (a folder named
    // after the mod) and whether it binds the server to 127.0.0.1 (config loopbackOnly, default on).
    public string? ModuleDir { get; init; }
    public string? ModuleName { get; init; }
    public string? ModulePlatform { get; init; }
    public bool ModuleLoads { get; init; }
    public bool LoopbackOnly { get; init; }
    // "missing" (no BDS), "leviLamina", "bedd" (no BeDD module); empty when ready.
    public IReadOnlyList<string> Problems { get; init; } = [];
    public bool Installed => !Problems.Contains("missing") && !Problems.Contains("leviLamina");
}

// What Servers.Start returns.
public sealed record ServerRun
{
    public required ServerInfo Server { get; init; }
    public int Pid { get; init; }
    public GameEndpoint? Endpoint { get; init; }
    public bool AlreadyRunning { get; init; }
}

// Bedrock Dedicated Servers for the client+server mode. lip installs LeviLamina's server package
// into an empty folder, and that package brings the official BDS of the game version with it
// (github.com/LiteLDev/bds). Mods of a server live in mods\. A server runs without any window: a
// hidden BeDD.exe ("server console-host") owns its console, writes what it prints to
// logs\console.log and passes "stop" to it when asked through a named event.
public static partial class Servers
{
    // The first port Init hands out (IPv4; IPv6 is the next one). 19132, Bedrock's default, is left
    // to the games and servers started by hand.
    public const int BasePort = 19140;

    // The mutex every BeDD setup on the PC holds: two servers set up at once would pick the same
    // ports.
    public const string SetupMutex = @"Global\BeDD.Setup";

    // Config keys of the pipe-era module, gone with the WebSocket transport.
    internal static readonly string[] ObsoleteConfigKeys = ["pipeName", "pipeInstances"];

    private static readonly string[] ProcessNames = ["bedrock_server_mod", "bedrock_server"];

    [GeneratedRegex(@"^[A-Za-z0-9._-]+$")]
    private static partial Regex NamePattern();

    [GeneratedRegex(@"^\s*([^#=\s][^=]*?)\s*=(.*)$")]
    private static partial Regex PropertyLine();

    [GeneratedRegex(@"ERR \[LeviLamina\][^\r\n]*BeDD")]
    private static partial Regex LeviLaminaBeDDError();

    public static bool IsValidName(string name) => NamePattern().IsMatch(name) && name is not ("." or "..");

    public static string DirOf(string name, string? serversDir = null) => Path.Combine(serversDir ?? Paths.Servers, name);

    // --- server.properties ------------------------------------------------------------------

    // key=value lines of a server.properties, in file order (comments left out).
    public static Dictionary<string, string> ReadProperties(string path)
    {
        var p = new Dictionary<string, string>(StringComparer.Ordinal);
        if (!File.Exists(path))
            return p;
        foreach (var line in File.ReadAllLines(path))
        {
            var m = PropertyLine().Match(line);
            if (m.Success)
                p[m.Groups[1].Value] = m.Groups[2].Value.Trim();
        }
        return p;
    }

    // Sets keys of a server.properties in place (comments and other keys stay), adding the ones it
    // lacks. Writes only when something changed; returns whether it did.
    public static bool SetProperties(string path, IEnumerable<KeyValuePair<string, string>> values)
    {
        var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : [];
        var left = new List<KeyValuePair<string, string>>();
        foreach (var kv in values)
        {
            left.RemoveAll(e => e.Key == kv.Key);
            left.Add(kv);
        }
        bool changed = false;
        for (int i = 0; i < lines.Count; i++)
        {
            var m = PropertyLine().Match(lines[i]);
            if (!m.Success)
                continue;
            string k = m.Groups[1].Value;
            int at = left.FindIndex(e => e.Key == k);
            if (at < 0)
                continue;
            string v = left[at].Value;
            if (m.Groups[2].Value.Trim() != v)
            {
                lines[i] = $"{k}={v}";
                changed = true;
            }
            left.RemoveAt(at);
        }
        foreach (var kv in left)
        {
            lines.Add($"{kv.Key}={kv.Value}");
            changed = true;
        }
        if (changed)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
            File.WriteAllLines(path, lines, new UTF8Encoding(false));
        }
        return changed;
    }

    // The keys a new server gets (from the PowerShell client's Initialize-BeDDServer): NetherNet on
    // loopback (since 1.26.60 the game joins a server by address over NetherNet only), no Xbox
    // authentication, creative and peaceful with cheats, not announced on the LAN.
    public static List<KeyValuePair<string, string>> DefaultProperties(string name) =>
    [
        new("server-name", "BeDD " + name),
        new("transport", "nethernet"),
        new("server-ip", "127.0.0.1"),
        new("online-mode", "false"),
        new("allow-list", "false"),
        new("level-name", "Bedrock level"),
        new("gamemode", "creative"),
        new("difficulty", "peaceful"),
        new("max-players", "10"),
        new("allow-cheats", "true"),
        new("enable-lan-visibility", "false"),
    ];

    // NetherNet: the UDP address clients send to once the signaling on server-port is done. The
    // module binds the server's UDP sockets to loopback, so with server-ip 127.0.0.1 that is
    // 127.0.0.1:<port> (one port, the same number as server-port); otherwise the server publishes
    // its interfaces' addresses.
    internal static string UdpPorts(string? transport, string? serverIp, int port) =>
        transport == "nethernet" && serverIp == "127.0.0.1" ? $"127.0.0.1:{port}:{port}" : "";

    // --- inspection ---------------------------------------------------------------------------

    public static ServerInfo Get(string name, string? serversDir = null)
    {
        string dir = Path.GetFullPath(DirOf(name, serversDir));
        var props = ReadProperties(Path.Combine(dir, "server.properties"));
        var problems = new List<string>();
        if (!File.Exists(Path.Combine(dir, "bedrock_server.exe")) && !File.Exists(Path.Combine(dir, "bedrock_server.exe.bak")))
            problems.Add("missing");
        if (!File.Exists(Path.Combine(dir, "bedrock_server_mod.exe")) || !File.Exists(Path.Combine(dir, "mods", "LeviLamina", "LeviLamina.dll")))
            problems.Add("leviLamina");
        // The one LeviLamina loads (a folder named after the mod) first.
        var module = FindModules(Path.Combine(dir, "mods"))
            .OrderByDescending(m => string.Equals(Path.GetFileName(m.Dir), m.Name, StringComparison.Ordinal))
            .FirstOrDefault();
        bool loopback = false;
        if (module.Dir is not null)
        {
            loopback = true;
            var cfg = ReadJson(Path.Combine(module.Dir, "config", "config.json"));
            if (cfg?["loopbackOnly"] is JsonValue v && v.TryGetValue(out bool b) && !b)
                loopback = false;
        }
        else
        {
            problems.Add("bedd");
        }
        return new ServerInfo
        {
            Name = name,
            Dir = dir,
            Port = IntOf(props, "server-port"),
            PortV6 = IntOf(props, "server-portv6"),
            ModuleDir = module.Dir,
            ModuleName = module.Name,
            ModulePlatform = module.Platform,
            ModuleLoads = module.Dir is not null && string.Equals(Path.GetFileName(module.Dir), module.Name, StringComparison.Ordinal),
            LoopbackOnly = loopback,
            Problems = problems,
        };
    }

    // Every folder under the servers folder, by name.
    public static List<ServerInfo> List(string? serversDir = null)
    {
        serversDir ??= Paths.Servers;
        if (!Directory.Exists(serversDir))
            return [];
        return Directory.EnumerateDirectories(serversDir)
            .Select(Path.GetFileName)
            .Where(n => n is not null && IsValidName(n))
            .Order(StringComparer.OrdinalIgnoreCase)
            .Select(n => Get(n!, serversDir))
            .ToList();
    }

    private static int IntOf(Dictionary<string, string> props, string key) =>
        props.TryGetValue(key, out var s) && int.TryParse(s, System.Globalization.CultureInfo.InvariantCulture, out int n) ? n : 0;

    internal static JsonObject? ReadJson(string path)
    {
        try
        {
            return JsonNode.Parse(File.ReadAllText(path)) as JsonObject;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    // A JSON string, or null for anything else.
    internal static string? Str(JsonNode? node) =>
        node is JsonValue v && v.TryGetValue(out string? s) ? s : null;

    // BeDD modules in a mods folder: each subfolder BeDD* whose manifest.json names a mod BeDD*.
    internal static IEnumerable<(string Dir, string Name, string? Platform)> FindModules(string modsDir)
    {
        if (!Directory.Exists(modsDir))
            yield break;
        foreach (var d in Directory.EnumerateDirectories(modsDir, "BeDD*"))
        {
            var m = ReadJson(Path.Combine(d, "manifest.json"));
            string? name = Str(m?["name"]);
            if (name is not null && name.StartsWith("BeDD", StringComparison.OrdinalIgnoreCase))
                yield return (d, name, Str(m!["platform"]));
        }
    }

    // Ids of the server processes running from dir (matched by their exe's folder).
    public static List<int> FindProcesses(string dir)
    {
        string prefix = Path.GetFullPath(dir).TrimEnd('\\') + "\\";
        var result = new List<int>();
        foreach (var name in ProcessNames)
        {
            foreach (var p in Process.GetProcessesByName(name))
            {
                using (p)
                {
                    var path = ImagePath(p.Id);
                    if (path is not null && path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
                        !path[prefix.Length..].Contains('\\'))
                        result.Add(p.Id);
                }
            }
        }
        return result;
    }

    // The full path of a process's exe, or null when it cannot be read.
    internal static string? ImagePath(int pid)
    {
        using var h = PInvoke.OpenProcess_SafeHandle(PROCESS_ACCESS_RIGHTS.PROCESS_QUERY_LIMITED_INFORMATION, false, (uint)pid);
        if (h.IsInvalid)
            return null;
        Span<char> buf = stackalloc char[1024];
        uint size = (uint)buf.Length;
        if (!PInvoke.QueryFullProcessImageName(h, PROCESS_NAME_FORMAT.PROCESS_NAME_WIN32, buf, ref size))
            return null;
        return new string(buf[..(int)size]);
    }

    // The endpoint a running server announces in Paths.Run (kind "server", its pid), if any.
    public static GameEndpoint? EndpointOf(int pid, string? runDir = null) =>
        Discovery.List(runDir: runDir).FirstOrDefault(e => e.Pid == pid && e.Kind == "server");

    // Waits until the server pid announces its endpoint, it exits, or the deadline passes.
    public static GameEndpoint? WaitForEndpoint(int pid, DateTime deadlineUtc, string? runDir = null, Func<int, bool>? alive = null)
    {
        alive ??= Discovery.IsAlive;
        while (true)
        {
            var ep = EndpointOf(pid, runDir);
            if (ep is not null)
                return ep;
            if (!alive(pid) || DateTime.UtcNow >= deadlineUtc)
                return null;
            Thread.Sleep(250);
        }
    }

    // --- ports ---------------------------------------------------------------------------------

    // A port pair (IPv4, IPv6 = +1) no other BeDD server uses and nothing listens on: the first
    // free one of 19140, 19142, 19144, ...
    public static int FindFreePort(string serversDir, string? except, ISet<int>? listening = null)
    {
        listening ??= ListeningUdpPorts();
        var taken = new HashSet<int>();
        if (Directory.Exists(serversDir))
        {
            foreach (var d in Directory.EnumerateDirectories(serversDir))
            {
                if (string.Equals(Path.GetFileName(d), except, StringComparison.OrdinalIgnoreCase))
                    continue;
                var p = ReadProperties(Path.Combine(d, "server.properties"));
                foreach (var k in (string[])["server-port", "server-portv6"])
                    if (p.TryGetValue(k, out var s) && int.TryParse(s, out int n))
                        taken.Add(n);
            }
        }
        for (int port = BasePort; port < 65534; port += 2)
            if (!taken.Contains(port) && !taken.Contains(port + 1) && !listening.Contains(port) && !listening.Contains(port + 1))
                return port;
        throw new IOException("no free port pair for a server");
    }

    public static HashSet<int> ListeningUdpPorts()
    {
        try
        {
            return IPGlobalProperties.GetIPGlobalProperties().GetActiveUdpListeners().Select(e => e.Port).ToHashSet();
        }
        catch (NetworkInformationException)
        {
            return [];
        }
    }

    // --- the BeDD server module ------------------------------------------------------------------

    // Where the BeDD server module comes from when --from is not given: BEDD_SERVER_MOD_DIR, the
    // release layout (<folder of BeDD.exe>\mod\BeDD-server), or the nearest bin\BeDD-server up from
    // the current folder (a build tree). Null when there is none.
    public static string? FindModSource(string? from, string? envDir, string? exeDir, string? cwd)
    {
        if (!string.IsNullOrWhiteSpace(from))
            return Path.GetFullPath(from);
        if (!string.IsNullOrWhiteSpace(envDir))
            return Path.GetFullPath(envDir);
        if (exeDir is not null)
        {
            var release = Path.Combine(exeDir, "mod", "BeDD-server");
            if (File.Exists(Path.Combine(release, "manifest.json")))
                return release;
        }
        for (var d = cwd is null ? null : new DirectoryInfo(cwd); d is not null; d = d.Parent)
        {
            var built = Path.Combine(d.FullName, "bin", "BeDD-server");
            if (File.Exists(Path.Combine(built, "manifest.json")))
                return built;
        }
        return null;
    }

    // Copies the BeDD server module (moduleDir: manifest.json and the DLL it names) into
    // <mods>\<its name> when it differs, moving the config of a BeDD module installed under another
    // name there and removing that one. Returns whether it changed anything.
    public static bool CopyModule(string moduleDir, string modsDir, Func<bool> running)
    {
        var manifestPath = Path.Combine(moduleDir, "manifest.json");
        var m = ReadJson(manifestPath) ?? throw new FileNotFoundException(
            $"no manifest.json in {moduleDir} (the BeDD server module, bin\\BeDD-server)");
        string name = Str(m["name"]) ?? "";
        if (!name.StartsWith("BeDD", StringComparison.OrdinalIgnoreCase) || !IsValidName(name))
            throw new InvalidDataException($"{manifestPath} is not a BeDD module ('{name}')");
        string? platform = Str(m["platform"]);
        if (!string.IsNullOrEmpty(platform) && platform != "server")
            throw new InvalidDataException($"{moduleDir} is the BeDD module for '{platform}', not the server one (bin\\BeDD-server)");
        string dest = Path.Combine(modsDir, name);
        var others = FindModules(modsDir).Where(o => !string.Equals(Path.GetFullPath(o.Dir), Path.GetFullPath(dest),
            StringComparison.OrdinalIgnoreCase)).ToList();
        string entry = Str(m["entry"]) is { Length: > 0 } e ? e : "BeDD.dll";
        if (entry != Path.GetFileName(entry))
            throw new InvalidDataException($"{manifestPath}: entry '{entry}' is not a file name");
        var files = new List<string> { manifestPath, Path.Combine(moduleDir, entry) };
        var pdb = Path.Combine(moduleDir, Path.ChangeExtension(entry, ".pdb"));
        if (File.Exists(pdb))
            files.Add(pdb);
        var stale = new List<string>();
        foreach (var f in files)
        {
            if (!File.Exists(f))
                throw new FileNotFoundException($"{f} is missing (the BeDD server module)");
            var t = Path.Combine(dest, Path.GetFileName(f));
            if (!File.Exists(t) || !SameContent(f, t))
                stale.Add(f);
        }
        if (stale.Count == 0 && others.Count == 0)
            return false;
        if (running())
            throw new IOException("the server is running: stop it to update its BeDD module");
        Directory.CreateDirectory(dest);
        foreach (var o in others)
        {
            var cfg = Path.Combine(o.Dir, "config");
            var destCfg = Path.Combine(dest, "config");
            if (Directory.Exists(cfg) && !Directory.Exists(destCfg))
                CopyDir(cfg, destCfg);
            Directory.Delete(o.Dir, recursive: true);
        }
        foreach (var f in stale)
            File.Copy(f, Path.Combine(dest, Path.GetFileName(f)), overwrite: true);
        return true;
    }

    // Removes the config keys of the pipe-era module from <moduleDir>\config\config.json. Returns
    // whether it changed the file.
    public static bool CleanModuleConfig(string moduleDir)
    {
        var file = Path.Combine(moduleDir, "config", "config.json");
        var c = ReadJson(file);
        if (c is null)
            return false;
        bool changed = false;
        foreach (var k in ObsoleteConfigKeys)
            changed |= c.Remove(k);
        if (changed)
            File.WriteAllText(file, c.ToJsonString(new JsonSerializerOptions { WriteIndented = true }), new UTF8Encoding(false));
        return changed;
    }

    private static bool SameContent(string a, string b)
    {
        if (new FileInfo(a).Length != new FileInfo(b).Length)
            return false;
        using var fa = File.OpenRead(a);
        using var fb = File.OpenRead(b);
        return SHA256.HashData(fa).AsSpan().SequenceEqual(SHA256.HashData(fb));
    }

    private static void CopyDir(string src, string dst)
    {
        Directory.CreateDirectory(dst);
        foreach (var f in Directory.EnumerateFiles(src))
            File.Copy(f, Path.Combine(dst, Path.GetFileName(f)));
        foreach (var d in Directory.EnumerateDirectories(src))
            CopyDir(d, Path.Combine(dst, Path.GetFileName(d)));
    }

    // --- setup -----------------------------------------------------------------------------------

    public sealed record InitOptions
    {
        public required string Name { get; init; }
        public string? ServersDir { get; init; }
        // 0: what the server already has, else the first free pair from BasePort.
        public int Port { get; init; }
        // The BeDD server module to install (null: keep what the server has).
        public string? ModuleDir { get; init; }
        // More server.properties keys, applied last.
        public IReadOnlyList<KeyValuePair<string, string>> Properties { get; init; } = [];
        public TimeSpan LockTimeout { get; init; } = TimeSpan.FromMinutes(35);
    }

    // Makes a BDS with LeviLamina (and the BeDD server module) ready in <servers>\<Name> and returns
    // it. Fast when nothing is missing; one setup at a time on the PC.
    public static ServerInfo Init(InitOptions o, Action<string> notice, Action<string> warn)
    {
        string serversDir = Path.GetFullPath(o.ServersDir ?? Paths.Servers);
        using var mutex = new Mutex(false, SetupMutex);
        bool owned;
        try
        {
            owned = mutex.WaitOne(o.LockTimeout);
        }
        catch (AbandonedMutexException)
        {
            owned = true;
        }
        if (!owned)
            throw new TimeoutException("another BeDD setup did not finish");
        try
        {
            return InitLocked(o, serversDir, notice, warn);
        }
        finally
        {
            mutex.ReleaseMutex();
        }
    }

    private static ServerInfo InitLocked(InitOptions o, string serversDir, Action<string> notice, Action<string> warn)
    {
        var server = Get(o.Name, serversDir);
        if (!server.Installed)
        {
            if (FindProcesses(server.Dir).Count > 0)
                throw new IOException($"the server in {server.Dir} is running: stop it first");
            Directory.CreateDirectory(server.Dir);
            // The server package (no #client): LeviLamina and, as its dependency, the BDS itself.
            string lip = Lip.Find(notice);
            string? dotnetRoot = Lip.EnsureDotnetRuntime(notice);
            string package = $"github.com/LiteLDev/LeviLamina@{BuildInfo.LeviLaminaVersion}";
            notice($"installing BDS {BuildInfo.GameVersion} and LeviLamina {BuildInfo.LeviLaminaVersion} into {server.Dir} with lip (about 100 MB the first time)");
            int code = Lip.Run(lip, ["install", package], server.Dir, dotnetRoot, notice);
            if (code != 0)
                throw new IOException($"lip install {package} failed in {server.Dir} (exit code {code})");
            server = Get(o.Name, serversDir);
            if (!server.Installed)
                throw new IOException($"{server.Dir} is not ready after lip install ({string.Join(", ", server.Problems)})");
        }

        // server.properties. A fresh BDS (before this wrote its server-name) gets BeDD's defaults and
        // a free port pair: its 19132/19133 are not its own. Later runs change only what is asked.
        var current = ReadProperties(server.PropertiesFile);
        // A fresh BDS has the name and ports it ships with; a server-name changed with --set still
        // leaves BeDD's port.
        bool ours = (current.TryGetValue("server-name", out var sn) && sn == "BeDD " + o.Name) ||
            (server.Port > 0 && server.Port != 19132);
        var values = ours ? [] : DefaultProperties(o.Name);
        int port = o.Port;
        if (port == 0)
            port = ours && server.Port > 0 ? server.Port : FindFreePort(serversDir, o.Name);
        int portV6 = ours && server.PortV6 > 0 && server.Port == port ? server.PortV6 : port + 1;
        values.Add(new("server-port", port.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        values.Add(new("server-portv6", portV6.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        values.AddRange(o.Properties);
        // server-udp-ports follows the port unless given.
        if (!o.Properties.Any(p => p.Key == "server-udp-ports"))
        {
            string Final(string key) => values.LastOrDefault(v => v.Key == key).Value ?? current.GetValueOrDefault(key) ?? "";
            values.Add(new("server-udp-ports", UdpPorts(Final("transport"), Final("server-ip"), int.TryParse(Final("server-port"), System.Globalization.CultureInfo.InvariantCulture, out int p) ? p : 0)));
        }
        if (FindProcesses(server.Dir).Count > 0)
        {
            var differs = values.Where(v => current.GetValueOrDefault(v.Key) != v.Value).Select(v => v.Key).Distinct().ToList();
            if (differs.Count > 0)
                warn($"server {o.Name} is running: {string.Join(", ", differs)} take effect at its next start");
        }
        if (SetProperties(server.PropertiesFile, values))
            notice($"server {o.Name}: server.properties written (port {port}/{portV6})");

        // The BeDD server module.
        if (o.ModuleDir is not null)
        {
            string dir = server.Dir;
            if (CopyModule(o.ModuleDir, server.ModsDir, () => FindProcesses(dir).Count > 0))
                notice($"server {o.Name}: BeDD module installed from {o.ModuleDir}");
            server = Get(o.Name, serversDir);
        }
        else if (server.Problems.Contains("bedd"))
        {
            notice($"server {o.Name}: no BeDD module (no --from, BEDD_SERVER_MOD_DIR or bin\\BeDD-server): 'server start' refuses to start it without one");
        }
        if (server.ModuleDir is not null && CleanModuleConfig(server.ModuleDir))
            notice($"server {o.Name}: removed the pipe settings from the BeDD config");
        return Get(o.Name, serversDir);
    }

    // --- start / stop ----------------------------------------------------------------------------

    // The named event a server's console host waits on to pass "stop" to the server, one per server
    // folder.
    internal static string StopEventName(string dir)
    {
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(dir).TrimEnd('\\').ToUpperInvariant()));
        return @"Global\BeDD.Server." + Convert.ToHexString(hash, 0, 8) + ".Stop";
    }

    // Why a server must not start without allowFirewallPrompt (a BDS without BeDD's loopback
    // binding listens on the network and Windows Firewall asks on the desktop), or null.
    public static string? FirewallProblem(ServerInfo s)
    {
        if (s.Problems.Contains("bedd"))
            return "has no BeDD server module (server init --from bin\\BeDD-server)";
        if (!string.IsNullOrEmpty(s.ModulePlatform) && s.ModulePlatform != "server")
            return $"has a BeDD module for '{s.ModulePlatform}' in {s.ModuleDir}, not the server one (bin\\BeDD-server)";
        if (!s.ModuleLoads)
            return $"has its BeDD module in {s.ModuleDir}, a folder not named after the mod, which LeviLamina does not load (server init --from moves it)";
        if (!s.LoopbackOnly)
            return $"has loopbackOnly: false in {s.ModuleDir}\\config\\config.json";
        return null;
    }

    // Starts the server with no window (its console host is a hidden BeDD.exe) and waits until it is
    // up ("Server started" in its log) and, unless detach, until it announces its WebSocket
    // endpoint. A server already running is returned as it is.
    public static ServerRun Start(string name, bool detach, TimeSpan timeout, bool allowFirewallPrompt,
        Action<string> notice, string? serversDir = null)
    {
        var server = Get(name, serversDir);
        var deadline = DateTime.UtcNow + timeout;
        var running = FindProcesses(server.Dir);
        if (running.Count > 0)
        {
            int pid = running[0];
            var ep = detach ? EndpointOf(pid) : WaitForEndpoint(pid, deadline);
            if (!detach && ep is null && server.ModuleDir is not null)
                throw new TimeoutException($"server {name} is running (pid {pid}) but announced no BeDD endpoint in {Paths.Run}");
            return new ServerRun { Server = server, Pid = pid, Endpoint = ep, AlreadyRunning = true };
        }
        if (!server.Installed)
            throw new InvalidOperationException($"server {name} is not set up in {server.Dir} ({string.Join(", ", server.Problems)})");
        if (!allowFirewallPrompt && FirewallProblem(server) is { } why)
            throw new InvalidOperationException($"not starting server {name}: it {why}, so it would listen on the network and Windows Firewall would ask on the desktop");
        if (allowFirewallPrompt && server.Problems.Contains("bedd"))
            notice($"server {name} has no BeDD module: starting it without BeDD");

        Directory.CreateDirectory(Path.GetDirectoryName(server.LogPath)!);
        if (File.Exists(server.LogPath))
            File.Delete(server.LogPath);
        string self = Environment.ProcessPath ?? throw new InvalidOperationException("the path of BeDD.exe is unknown");
        int hostPid = StartDetached(self, $"server console-host \"{server.Dir}\"", server.Dir);
        notice($"starting server {name} on port {server.Port} (log: {server.LogPath})");

        int serverPid = 0;
        bool started = false;
        string text = "";
        while (DateTime.UtcNow < deadline)
        {
            if (serverPid == 0)
                serverPid = FindProcesses(server.Dir).FirstOrDefault();
            text = ReadShared(server.LogPath);
            // LeviLamina did not load the module after all: stop the server before it opens its
            // network.
            if (!allowFirewallPrompt && LeviLaminaBeDDError().IsMatch(text))
            {
                KillAll(server.Dir);
                var errors = text.Split('\n').Where(l => l.Contains("ERR [LeviLamina]", StringComparison.Ordinal)).Select(l => l.TrimEnd());
                throw new InvalidOperationException($"LeviLamina did not load BeDD in server {name}: stopped it before it listens on the network. See {server.LogPath}:\n{string.Join("\n", errors)}");
            }
            if (text.Contains("Server started", StringComparison.Ordinal))
            {
                started = true;
                break;
            }
            if (!Discovery.IsAlive(hostPid))
                break;
            Thread.Sleep(500);
        }
        if (!started)
        {
            var tail = text.Length > 0 ? string.Join("\n", text.Split('\n').Select(l => l.TrimEnd()).Where(l => l.Length > 0).TakeLast(20)) : "(no log)";
            // Nothing left behind: the server (its console host exits with it), or the host when the
            // server never showed up.
            KillAll(server.Dir);
            Kill(hostPid);
            throw new TimeoutException($"server {name} did not start within {timeout.TotalSeconds:0} s. Last lines of {server.LogPath}:\n{tail}");
        }
        if (serverPid == 0)
            serverPid = FindProcesses(server.Dir).FirstOrDefault();
        if (serverPid == 0)
        {
            Kill(hostPid);
            throw new InvalidOperationException($"server {name} started but its process in {server.Dir} cannot be found");
        }
        GameEndpoint? endpoint = null;
        if (!detach && server.ModuleDir is not null)
        {
            endpoint = WaitForEndpoint(serverPid, deadline);
            if (endpoint is null)
                throw new TimeoutException($"server {name} is up (pid {serverPid}) but BeDD announced no endpoint in {Paths.Run} within {timeout.TotalSeconds:0} s and is left running (see {server.LogPath}; 'server stop {name}' stops it)");
        }
        else
        {
            endpoint = EndpointOf(serverPid);
        }
        return new ServerRun { Server = server, Pid = serverPid, Endpoint = endpoint };
    }

    // Sends "stop" to the server console (the server saves the world and exits) and kills it when
    // it has not exited within timeout. Returns false when it was not running.
    public static bool Stop(ServerInfo server, TimeSpan timeout, Action<string> notice, Action<string> warn)
    {
        var pids = FindProcesses(server.Dir);
        if (pids.Count == 0)
            return false;
        notice($"stopping server {server.Name} (pid {string.Join(", ", pids)})");
        if (EventWaitHandle.TryOpenExisting(StopEventName(server.Dir), out var ev))
        {
            using (ev)
                ev.Set();
        }
        else
        {
            warn($"server {server.Name} has no console host to pass 'stop' to: killing it");
            timeout = TimeSpan.Zero;
        }
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline && pids.Any(Discovery.IsAlive))
            Thread.Sleep(250);
        if (pids.Any(Discovery.IsAlive))
        {
            if (timeout > TimeSpan.Zero)
                warn($"server {server.Name} did not stop within {timeout.TotalSeconds:0} s: killing it");
            foreach (var pid in pids)
                Kill(pid);
            var killDeadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
            while (DateTime.UtcNow < killDeadline && pids.Any(Discovery.IsAlive))
                Thread.Sleep(100);
        }
        return true;
    }

    private static void KillAll(string dir)
    {
        foreach (var pid in FindProcesses(dir))
            Kill(pid);
    }

    private static void Kill(int pid)
    {
        try
        {
            using var p = Process.GetProcessById(pid);
            p.Kill();
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
        }
    }

    private static string ReadShared(string path)
    {
        try
        {
            using var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var r = new StreamReader(s, Encoding.UTF8);
            return r.ReadToEnd();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return "";
        }
    }

    // A process with no console window and none of this process's handles (a caller whose output
    // is captured, e.g. by CI, is not kept waiting for the server to exit). Returns its id.
    private static unsafe int StartDetached(string exe, string args, string dir)
    {
        var si = new Windows.Win32.System.Threading.STARTUPINFOW { cb = (uint)sizeof(Windows.Win32.System.Threading.STARTUPINFOW) };
        string cmd = $"\"{exe}\" {args}\0";
        var cmdBuf = cmd.ToCharArray();
        PROCESS_INFORMATION pi;
        fixed (char* pCmd = cmdBuf)
        fixed (char* pExe = exe)
        fixed (char* pDir = dir)
        {
            if (!PInvoke.CreateProcess(pExe, pCmd, null, null, false, PROCESS_CREATION_FLAGS.CREATE_NO_WINDOW,
                    null, pDir, &si, &pi))
                throw new System.ComponentModel.Win32Exception();
        }
        PInvoke.CloseHandle(pi.hThread);
        PInvoke.CloseHandle(pi.hProcess);
        return (int)pi.dwProcessId;
    }

    // --- the console host ------------------------------------------------------------------------

    [GeneratedRegex("\x1b\\[[0-9;?]*[A-Za-z]")]
    private static partial Regex Ansi();

    // Runs the server of dir with its console redirected: output (colours stripped) to its log,
    // "stop" to its stdin when the stop event is set. Returns the server's exit code once it exits.
    public static int RunConsoleHost(string dir)
    {
        dir = Path.GetFullPath(dir);
        var server = Get(Path.GetFileName(dir), Path.GetDirectoryName(dir));
        using var stopEvent = new EventWaitHandle(false, EventResetMode.AutoReset, StopEventName(dir), out bool createdNew);
        if (!createdNew)
            return 3; // another console host owns this server
        Directory.CreateDirectory(Path.GetDirectoryName(server.LogPath)!);
        using var log = new StreamWriter(new FileStream(server.LogPath, FileMode.Create, FileAccess.Write,
            FileShare.ReadWrite | FileShare.Delete), new UTF8Encoding(false)) { AutoFlush = true };
        object gate = new();
        void Write(string? line)
        {
            if (line is null)
                return;
            lock (gate)
                log.WriteLine(Ansi().Replace(line, ""));
        }
        var psi = new ProcessStartInfo(server.Exe)
        {
            WorkingDirectory = dir,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            StandardInputEncoding = new UTF8Encoding(false),
        };
        using var p = new Process { StartInfo = psi };
        p.OutputDataReceived += (_, e) => Write(e.Data);
        p.ErrorDataReceived += (_, e) => Write(e.Data);
        p.Start();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        var stdin = p.StandardInput;
        stdin.AutoFlush = true;
        var console = new Thread(() =>
        {
            while (true)
            {
                stopEvent.WaitOne();
                Write("> stop");
                try
                {
                    lock (gate)
                        stdin.WriteLine("stop");
                }
                catch (IOException)
                {
                    return; // the server is gone
                }
            }
        }) { IsBackground = true };
        console.Start();
        p.WaitForExit();
        Write($"[BeDD] server exited with code {p.ExitCode}");
        return p.ExitCode;
    }
}
