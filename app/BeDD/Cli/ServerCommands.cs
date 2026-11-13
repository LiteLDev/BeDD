using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Cli;

// server init|start|stop|list|remove|path: Bedrock Dedicated Servers with LeviLamina and the BeDD
// server module, for the client+server mode (Core/Servers.cs).
internal static class ServerCommands
{
    private const string P = Dispatcher.ProgramName;

    [ModuleInitializer]
    internal static void Register()
    {
        Registry.Register(new Command
        {
            Path = "server init",
            Summary = "set up a server: BDS and LeviLamina through lip, the BeDD server module, server.properties",
            Args = "<name>",
            MinArgs = 1,
            MaxArgs = 1,
            Description =
                "Makes a Bedrock Dedicated Server with LeviLamina " + BuildInfo.LeviLaminaVersion + " ready in the servers folder\n" +
                "(lip installs LeviLamina's server package, which brings the BDS of " + BuildInfo.GameVersion + "). Fast when\n" +
                "nothing is missing; run again to update the module or change settings.\n\n" +
                "A new server gets ports from " + Servers.BasePort + " up (IPv4, IPv6 = +1), NetherNet on 127.0.0.1, no\n" +
                "Xbox authentication, creative, peaceful, cheats on. The BeDD server module comes from --from,\n" +
                "BEDD_SERVER_MOD_DIR, <folder of BeDD.exe>\\mod\\BeDD-server or the nearest bin\\BeDD-server.",
            Flags =
            [
                Flag.Int("port", "", "N", "IPv4 port (IPv6 is N+1); default: the server's own, else the first free pair"),
                Flag.String("from", "", "DIR", "the BeDD server module to install (bin\\BeDD-server)"),
                Flag.Strings("set", "", "KEY=VALUE", "a server.properties key (repeatable)"),
            ],
            Examples = ["server init s1", "server init s1 --from bin\\BeDD-server --set level-name=test --set gamemode=survival"],
            Run = Init,
        });
        Registry.Register(new Command
        {
            Path = "server start",
            Summary = "start a server in the background and print its endpoint",
            Args = "<name>",
            MinArgs = 1,
            MaxArgs = 1,
            Description =
                "Starts the server with no window (a hidden BeDD.exe owns its console and writes it to\n" +
                "logs\\console.log), waits until it is up and, unless --detach, until BeDD announces its\n" +
                "WebSocket endpoint. Prints name, pid, port, portV6, url and token. A server already running is\n" +
                "printed as it is. A server without the BeDD module (which binds it to 127.0.0.1) is refused, as\n" +
                "Windows Firewall would ask on the desktop, unless --allow-firewall-prompt.",
            Flags =
            [
                Flag.Bool("detach", "", "do not wait for BeDD's endpoint (url and token may be null)"),
                Flag.Int("timeout", "", "SECONDS", "how long to wait", 180),
                Flag.Bool("allow-firewall-prompt", "", "start a server without BeDD's loopback binding"),
            ],
            Examples = ["server start s1", "server start s1 --detach --json"],
            Run = Start,
        });
        Registry.Register(new Command
        {
            Path = "server stop",
            Summary = "stop a server (it saves its world), or every one with --all",
            Args = "<name>|--all",
            MaxArgs = 1,
            Flags =
            [
                Flag.Bool("all", "", "stop every server in the servers folder"),
                Flag.Int("timeout", "", "SECONDS", "how long to wait before killing it", 30),
            ],
            Examples = ["server stop s1", "server stop --all"],
            Run = Stop,
        });
        Registry.Register(new Command
        {
            Path = "server list",
            Summary = "list the servers, their ports and whether they run",
            Aliases = ["ls"],
            Examples = ["server list", "server list --json"],
            Run = List,
        });
        Registry.Register(new Command
        {
            Path = "server remove",
            Summary = "delete a stopped server's folder (its world too)",
            Args = "<name>",
            MinArgs = 1,
            MaxArgs = 1,
            Aliases = ["rm"],
            Flags = [Flag.Bool("yes", "y", "do not ask; needed without a console")],
            Examples = ["server remove s1 --yes"],
            Run = Remove,
        });
        Registry.Register(new Command
        {
            Path = "server path",
            Summary = "print the folder of a server",
            Args = "<name>",
            MinArgs = 1,
            MaxArgs = 1,
            Examples = ["server path s1"],
            Run = PathOf,
        });
        Registry.Register(new Command
        {
            Path = "server console-host",
            Summary = "internal: owns the console of a server 'server start' started",
            Args = "<dir>",
            MinArgs = 1,
            MaxArgs = 1,
            Run = ctx =>
            {
                // Started by 'server start' without a console; never stay on the caller's one, whose
                // closing would end the server with it.
                Windows.Win32.PInvoke.FreeConsole();
                return Servers.RunConsoleHost(ctx.Arg(0)!);
            },
        });
    }

    private static string Name(Ctx ctx)
    {
        string name = ctx.Arg(0)!;
        if (!Servers.IsValidName(name))
            throw CliError.Usage($"server name '{name}': letters, digits, '.', '_' and '-' only");
        return name;
    }

    private static ServerInfo Existing(string name)
    {
        var s = Servers.Get(name);
        if (!Directory.Exists(s.Dir))
            throw CliError.NotFound("ERR_SERVER_NOT_FOUND", $"no server '{name}' in {Paths.Servers}", $"{P} server init {name}");
        return s;
    }

    // Core reports what goes wrong with plain exceptions; these are the command line's errors.
    private static T Guard<T>(Func<T> f)
    {
        try
        {
            return f();
        }
        catch (TimeoutException e)
        {
            throw CliError.Failure("ERR_TIMEOUT", e.Message);
        }
        catch (InvalidOperationException e)
        {
            throw CliError.Failure("ERR_SERVER_NOT_READY", e.Message);
        }
        catch (InvalidDataException e)
        {
            throw CliError.Failure("ERR_BAD_MODULE", e.Message);
        }
        catch (HttpRequestException e)
        {
            throw CliError.Failure("ERR_DOWNLOAD", "download failed: " + e.Message);
        }
    }

    internal static List<KeyValuePair<string, string>> ParseSets(IReadOnlyList<string> sets)
    {
        var result = new List<KeyValuePair<string, string>>();
        foreach (var s in sets)
        {
            int eq = s.IndexOf('=');
            string key = eq > 0 ? s[..eq].Trim() : "";
            string value = eq > 0 ? s[(eq + 1)..].Trim() : "";
            if (key.Length == 0 || key.StartsWith('#') || key.Any(char.IsWhiteSpace) || value.Contains('\n') || value.Contains('\r'))
                throw CliError.Usage($"--set takes key=value, not '{s}'");
            result.Add(new(key, value));
        }
        return result;
    }

    private static int Init(Ctx ctx)
    {
        string name = Name(ctx);
        int port = ctx.Int("port") ?? 0;
        if (port != 0 && (port < 1 || port > 65534))
            throw CliError.Usage("--port must be between 1 and 65534");
        var sets = ParseSets(ctx.Strings("set"));
        string? from = Servers.FindModSource(ctx.String("from"), Environment.GetEnvironmentVariable("BEDD_SERVER_MOD_DIR"),
            AppContext.BaseDirectory, Environment.CurrentDirectory);
        if (ctx.String("from") is { } given && !Directory.Exists(from))
            throw CliError.NotFound("ERR_MODULE_NOT_FOUND", $"no folder {given}");
        if (from is not null)
            ctx.Verbose($"BeDD server module: {from}");
        var server = Guard(() => Servers.Init(new Servers.InitOptions
        {
            Name = name,
            Port = port,
            ModuleDir = from,
            Properties = sets,
        }, ctx.Notice, ctx.Warn));
        var pid = Servers.FindProcesses(server.Dir).FirstOrDefault();
        return ctx.Render(ServerJson(server, pid, pid > 0 ? Servers.EndpointOf(pid) : null), [server.Name], w =>
        {
            w.WriteLine($"server {server.Name} ready in {server.Dir} (port {server.Port}/{server.PortV6})");
            if (server.Problems.Count > 0)
                w.WriteLine($"lacks: {string.Join(", ", server.Problems)}");
        });
    }

    private static int Start(Ctx ctx)
    {
        string name = Name(ctx);
        Existing(name);
        int timeout = ctx.Int("timeout") ?? 180;
        if (timeout <= 0)
            throw CliError.Usage("--timeout must be positive");
        var run = Guard(() => Servers.Start(name, ctx.Bool("detach"), TimeSpan.FromSeconds(timeout),
            ctx.Bool("allow-firewall-prompt"), ctx.Notice));
        if (run.AlreadyRunning)
            ctx.Notice($"server {name} is already running (pid {run.Pid})");
        var s = run.Server;
        var json = new JsonObject
        {
            ["name"] = s.Name,
            ["pid"] = run.Pid,
            ["port"] = s.Port,
            ["portV6"] = s.PortV6,
            ["url"] = run.Endpoint?.Url,
            ["token"] = run.Endpoint?.Token,
            ["dir"] = s.Dir,
            ["logPath"] = s.LogPath,
        };
        return ctx.Render(json, [s.Name], w =>
        {
            w.WriteLine($"server {s.Name} is up on port {s.Port} (pid {run.Pid})");
            if (run.Endpoint is { } ep)
            {
                w.WriteLine($"url    {ep.Url}");
                w.WriteLine($"token  {ep.Token}");
            }
        });
    }

    private static int Stop(Ctx ctx)
    {
        bool all = ctx.Bool("all");
        if (all == (ctx.Args.Count == 1))
            throw CliError.Usage("server stop needs a server name or --all", $"{P} server stop --help");
        int timeout = ctx.Int("timeout") ?? 30;
        if (timeout < 0)
            throw CliError.Usage("--timeout must not be negative");
        var targets = all ? Servers.List() : [Existing(Name(ctx))];
        var stopped = new JsonArray();
        var names = new List<string>();
        foreach (var s in targets)
        {
            if (Servers.Stop(s, TimeSpan.FromSeconds(timeout), ctx.Notice, ctx.Warn))
            {
                stopped.Add((JsonNode)s.Name);
                names.Add(s.Name);
            }
            else if (!all)
            {
                ctx.Notice($"server {s.Name} is not running");
            }
        }
        return ctx.Render(new JsonObject { ["stopped"] = stopped }, names, w =>
        {
            foreach (var n in names)
                w.WriteLine($"server {n} stopped");
        });
    }

    private static int List(Ctx ctx)
    {
        var servers = Servers.List();
        var rows = new JsonArray();
        var table = new List<IReadOnlyList<string>>();
        foreach (var s in servers)
        {
            int pid = Servers.FindProcesses(s.Dir).FirstOrDefault();
            var ep = pid > 0 ? Servers.EndpointOf(pid) : null;
            rows.Add((JsonNode)ServerJson(s, pid, ep));
            table.Add([s.Name, s.Port > 0 ? $"{s.Port}/{s.PortV6}" : "-", pid > 0 ? pid.ToString(System.Globalization.CultureInfo.InvariantCulture) : "-",
                ep?.Url ?? "-", s.Problems.Count > 0 ? string.Join(",", s.Problems) : "ready"]);
        }
        return ctx.Render(rows, servers.Select(s => s.Name), w =>
        {
            if (servers.Count == 0)
            {
                w.WriteLine($"no servers in {Paths.Servers}");
                return;
            }
            Ctx.Table(w, ["NAME", "PORT", "PID", "URL", "STATE"], table);
        });
    }

    private static int Remove(Ctx ctx)
    {
        string name = Name(ctx);
        var s = Existing(name);
        if (Servers.FindProcesses(s.Dir).Count > 0)
            throw CliError.Conflict("ERR_SERVER_RUNNING", $"server {name} is running", $"{P} server stop {name}");
        if (!ctx.Bool("yes"))
            throw new CliError(ExitCode.Declined, "ERR_NOT_CONFIRMED", $"removing server {name} deletes {s.Dir} with its world",
                $"{P} server remove {name} --yes");
        Directory.Delete(s.Dir, recursive: true);
        ctx.Notice($"server {name} removed");
        return ctx.Render(new JsonObject { ["removed"] = name, ["dir"] = s.Dir }, [name], null);
    }

    private static int PathOf(Ctx ctx)
    {
        var s = Existing(Name(ctx));
        return ctx.Render(new JsonObject { ["name"] = s.Name, ["dir"] = s.Dir }, [s.Dir], w => w.WriteLine(s.Dir));
    }

    private static JsonObject ServerJson(ServerInfo s, int pid, GameEndpoint? ep)
    {
        var problems = new JsonArray();
        foreach (var p in s.Problems)
            problems.Add((JsonNode)p);
        return new JsonObject
        {
            ["name"] = s.Name,
            ["dir"] = s.Dir,
            ["port"] = s.Port,
            ["portV6"] = s.PortV6,
            ["running"] = pid > 0,
            ["pid"] = pid > 0 ? pid : null,
            ["url"] = ep?.Url,
            ["token"] = ep?.Token,
            ["module"] = s.ModuleName,
            ["loopbackOnly"] = s.LoopbackOnly,
            ["problems"] = problems,
        };
    }
}
