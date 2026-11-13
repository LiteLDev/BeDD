using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Cli;

internal static class ProcessCommands
{
    [ModuleInitializer]
    internal static void Register()
    {
        Registry.Register(new Command
        {
            Path = "process list",
            Aliases = ["ls"],
            Summary = "list running BeDD games and servers with their WebSocket endpoints",
            Description =
                "Lists the games and servers whose mod announced its WebSocket endpoint (run\\<pid>.json).\n" +
                "--json prints each endpoint with its token: that is how a test finds the address to connect\n" +
                "to. --all also lists games of BeDD's instances that have not announced one (still loading, or\n" +
                "an older mod).",
            Flags = [Flag.Bool("all", "", "also list games under the instances folder without an endpoint")],
            Examples = ["ps", "ps --json", "process list --all"],
            Run = List,
        });
        Registry.Register(new Command
        {
            Path = "process stop",
            Summary = "stop the games of an instance (alias: stop)",
            Args = "[<instance>]",
            MaxArgs = 1,
            Description =
                "Kills the games of a BeDD instance (a folder name under the instances folder, or its path), the\n" +
                "game with --pid, or with --all every game of BeDD's instances. Minecraft processes outside the\n" +
                "instances folder are never touched, unless --pid names one that announced a BeDD endpoint.",
            Flags =
            [
                Flag.Int("pid", "", "<pid>", "stop this game"),
                Flag.Bool("all", "", "stop every game of BeDD's instances"),
                Flag.Bool("yes", "y", "do not ask before stopping every game"),
            ],
            Examples = ["stop 1.26.60.05-2", "stop --pid 12345", "stop --all --yes"],
            Run = Stop,
        });
    }

    // One running game or server: from its discovery file, or (status "loading") only its process.
    private sealed record Entry(GameEndpoint Endpoint, bool Announced);

    private static List<Entry> Running(bool all)
    {
        var list = Discovery.List(prune: true).Select(e => new Entry(e, true)).ToList();
        if (all)
        {
            var known = list.Select(e => e.Endpoint.Pid).ToHashSet();
            foreach (var g in Launcher.GamesUnder(Paths.Instances))
            {
                if (known.Contains(g.Pid))
                    continue;
                list.Add(new Entry(new GameEndpoint
                {
                    Pid = g.Pid,
                    Kind = "client",
                    Instance = Launcher.InstanceOf(g.Exe),
                    InstanceDir = Path.Combine(Paths.Instances, Launcher.InstanceOf(g.Exe)),
                }, false));
            }
        }
        return list;
    }

    private static int List(Ctx ctx)
    {
        var list = Running(ctx.Bool("all"));
        var json = new JsonArray();
        foreach (var e in list)
        {
            var node = (JsonObject)JsonSerializer.SerializeToNode(e.Endpoint, DiscoveryJson.Default.GameEndpoint)!;
            node["status"] = e.Announced ? "ready" : "loading";
            if (!e.Announced)
            {
                // Only what the process itself tells: no protocol or mod version known yet.
                node.Remove("protocol");
                node.Remove("modVersion");
            }
            json.Add((JsonNode)node);
        }
        return ctx.Render(json, list.Select(e => e.Endpoint.Pid.ToString(CultureInfo.InvariantCulture)), w =>
        {
            if (list.Count == 0)
            {
                ctx.Notice("no BeDD game runs");
                return;
            }
            Ctx.Table(w, ["PID", "INSTANCE", "LABEL", "URL"], list.Select(e => (IReadOnlyList<string>)
            [
                e.Endpoint.Pid.ToString(CultureInfo.InvariantCulture),
                e.Endpoint.Instance + (e.Endpoint.Kind == "server" ? " (server)" : ""),
                e.Endpoint.Label,
                e.Announced ? e.Endpoint.Url : "(loading)",
            ]));
        });
    }

    private sealed record Target(int Pid, string Instance);

    private static int Stop(Ctx ctx)
    {
        string? name = ctx.Arg(0);
        int? pid = ctx.Has("pid") ? ctx.Int("pid") : null;
        bool all = ctx.Bool("all");
        if ((name is not null ? 1 : 0) + (pid is not null ? 1 : 0) + (all ? 1 : 0) != 1)
            throw CliError.Usage("name one instance, --pid or --all", $"{Dispatcher.ProgramName} stop --help");

        var targets = new List<Target>();
        if (pid is int p)
            targets.Add(TargetForPid(p));
        else if (all)
        {
            Discovery.List(prune: true);
            foreach (var g in Launcher.GamesOf(Paths.Instances, anyUnder: true))
                targets.Add(new Target(g.Pid, Launcher.InstanceOf(g.Exe)));
            if (targets.Count > 0 && !ctx.Bool("yes") && !Confirm(ctx, $"stop {targets.Count} game(s): " +
                    string.Join(", ", targets.Select(t => $"{t.Instance} (pid {t.Pid})")) + "?"))
                throw new CliError(ExitCode.Declined, "ERR_DECLINED", "not stopped", $"{Dispatcher.ProgramName} stop --all --yes");
        }
        else
            targets.AddRange(TargetsForInstance(name!));

        var json = new JsonArray();
        var stopped = new List<string>();
        var failed = new List<string>();
        foreach (var t in targets)
        {
            bool ok = Launcher.Kill(t.Pid, TimeSpan.FromSeconds(10));
            json.Add((JsonNode)new JsonObject { ["pid"] = t.Pid, ["instance"] = t.Instance, ["stopped"] = ok });
            (ok ? stopped : failed).Add(t.Pid.ToString(CultureInfo.InvariantCulture));
            if (ok)
                ctx.Notice($"stopped {t.Instance} (pid {t.Pid})");
        }
        if (failed.Count > 0)
            throw CliError.Failure("ERR_STOP_FAILED", $"could not stop pid {string.Join(", ", failed)}" +
                (stopped.Count > 0 ? $" (stopped pid {string.Join(", ", stopped)})" : ""));
        if (targets.Count == 0)
            ctx.Notice(name is not null ? $"no game of {name} runs" : "no BeDD game runs");
        return ctx.Render(json, stopped, null);
    }

    // The games of an instance: a folder name under the instances folder or a path. Its
    // Minecraft.Windows processes count only inside the instances folder; elsewhere only games that
    // announced a BeDD endpoint for that folder do.
    private static List<Target> TargetsForInstance(string name)
    {
        bool isPath = name.IndexOfAny(['\\', '/', ':']) >= 0;
        string dir = Path.GetFullPath(isPath ? name : Path.Combine(Paths.Instances, name));
        string instance = Path.GetFileName(dir.TrimEnd('\\', '/'));
        Discovery.List(prune: true);
        var targets = Launcher.GamesOf(dir, byExe: Launcher.IsUnder(dir, Paths.Instances))
            .Select(g => new Target(g.Pid, instance)).ToList();
        if (targets.Count == 0 && !isPath && !Directory.Exists(dir))
            throw CliError.NotFound("ERR_INSTANCE_NOT_FOUND", $"no instance {name} in {Paths.Instances}", $"{Dispatcher.ProgramName} ps --all");
        return targets;
    }

    private static Target TargetForPid(int pid)
    {
        string? exe = Launcher.ImagePath(pid);
        if (exe is null)
            throw CliError.NotFound("ERR_NOT_RUNNING", $"no process {pid} (or no access to it)");
        if (string.Equals(Path.GetFileName(exe), Launcher.GameExe, StringComparison.OrdinalIgnoreCase) && Launcher.IsUnder(exe, Paths.Instances))
            return new Target(pid, Launcher.InstanceOf(exe));
        var file = Path.Combine(Paths.Run, pid.ToString(CultureInfo.InvariantCulture) + ".json");
        if (Discovery.Read(file) is { } ep && ep.Pid == pid && Launcher.IsBeDDProcess(ep))
            return new Target(pid, ep.Instance);
        throw CliError.Conflict("ERR_NOT_BEDD", $"pid {pid} ({exe}) is not a BeDD game", $"{Dispatcher.ProgramName} ps --all");
    }

    // Asks on the console; false without one (input redirected to nothing, or no console at all).
    private static bool Confirm(Ctx ctx, string question)
    {
        ctx.Err.Write(question + " [y/N] ");
        ctx.Err.Flush();
        string? answer;
        try
        {
            answer = Console.In.ReadLine();
            if (answer is null && !Console.IsInputRedirected)
            {
                // A GUI-subsystem process attached to its parent's console has no standard input.
                using var conin = new StreamReader(new FileStream("CONIN$", FileMode.Open, FileAccess.Read));
                answer = conin.ReadLine();
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            answer = null;
        }
        if (answer is null)
            ctx.Err.WriteLine();
        return answer?.Trim().ToLowerInvariant() is "y" or "yes";
    }
}
