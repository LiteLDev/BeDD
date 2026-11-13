using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Cli;

// instance list|show|clone|refresh|remove|path: BeDD's game instances, clones of a LeviLauncher instance.
internal static class InstanceCommands
{
    private static readonly Flag SlotFlag = Flag.Int("slot", "", "<n>", "the instance of this slot (the clone of the base instance)");

    [ModuleInitializer]
    internal static void Register()
    {
        Registry.Register(new Command
        {
            Path = "instance list",
            Aliases = ["ls"],
            Summary = "list BeDD's game instances",
            Description = "List BeDD's game instances (clones under %LOCALAPPDATA%\\BeDD\\instances) and what each still lacks.\n" +
                "With --bases, list the LeviLauncher instances a clone can be made from instead.",
            Flags =
            [
                Flag.Bool("bases", "", "list the LeviLauncher instances clones can be made from"),
                Flag.Bool("all", "", "with --bases: also the instances that cannot be cloned, and why"),
            ],
            Examples = ["instance list", "instance list --bases", "instance list --json"],
            Run = ctx => ctx.Bool("bases") ? ListBases(ctx) : List(ctx),
        });
        Registry.Register(new Command
        {
            Path = "instance show",
            Summary = "show an instance and what it still lacks",
            Args = "[<name>]",
            MaxArgs = 1,
            Flags = [SlotFlag],
            Examples = ["instance show 1.26.60.05-2", "instance show --slot 2 --json"],
            Run = Show,
        });
        Registry.Register(new Command
        {
            Path = "instance clone",
            Summary = "clone the LeviLauncher instance for a slot, or refresh the clone",
            Description = "Make <game version>-<slot> under %LOCALAPPDATA%\\BeDD\\instances from a LeviLauncher instance: the game files\n" +
                "hard-linked, its assets through junctions, its game data copied, LeviLamina copied. The LeviLauncher instance is\n" +
                "only read. An existing clone is refreshed instead; --force makes it again from scratch (its game data too).\n" +
                "The base is --base, else BEDD_INSTANCE, else LeviLauncher's default instance, else the first that fits.",
            Flags =
            [
                Flag.Int("slot", "", "<n>", "the slot (one game per slot)", 1),
                Flag.String("base", "", "<name>", "the LeviLauncher instance to clone"),
                Flag.Bool("force", "f", "remove the clone and make it again"),
            ],
            Examples = ["instance clone", "instance clone --slot 2", "instance clone --slot 2 --base \"My instance\" --force"],
            Run = Clone,
        });
        Registry.Register(new Command
        {
            Path = "instance refresh",
            Summary = "bring clones up to date with their LeviLauncher instance",
            Args = "[<name>]",
            MaxArgs = 1,
            Flags = [SlotFlag, Flag.Bool("all", "", "every instance (those with a game running are skipped)")],
            Examples = ["instance refresh 1.26.60.05-2", "instance refresh --all"],
            Run = Refresh,
        });
        Registry.Register(new Command
        {
            Path = "instance remove",
            Aliases = ["rm"],
            Summary = "delete an instance (the LeviLauncher instance stays as it is)",
            Args = "[<name>]",
            MaxArgs = 1,
            Flags = [SlotFlag, Flag.Bool("yes", "y", "do not ask")],
            Examples = ["instance remove 1.26.60.05-2", "instance remove --slot 2 --yes"],
            Run = Remove,
        });
        Registry.Register(new Command
        {
            Path = "instance path",
            Summary = "print a folder of an instance",
            Args = "<name> [dir|mods|logs|data|worlds]",
            Description = "Print a folder of an instance: dir (the instance), mods, logs, data (its game data, \"Minecraft Bedrock\")\n" +
                "or worlds (minecraftWorlds of its first player).",
            MinArgs = 1,
            MaxArgs = 2,
            Examples = ["instance path 1.26.60.05-2", "instance path 1.26.60.05-2 worlds"],
            Run = PathOf,
        });
    }

    // <name>, else --slot.
    internal static InstanceInfo Pick(Ctx ctx)
    {
        var name = ctx.Arg(0);
        int? slot = ctx.Int("slot");
        if (name is not null && slot is not null)
            throw CliError.Usage("give an instance name or --slot, not both");
        if (name is null && slot is null)
            throw CliError.Usage($"{ctx.Command.Path} needs an instance name or --slot", $"{Dispatcher.ProgramName} instance list");
        return InstanceStore.Require(name, slot ?? 1);
    }

    private static JsonObject Describe(InstanceInfo i)
    {
        var o = i.ToJson();
        o["running"] = new JsonArray(InstanceStore.RunningGames(i).Select(p => (JsonNode?)p).ToArray());
        return o;
    }

    private static string Status(InstanceInfo i) => i.Problems.Count == 0 ? "ready" : string.Join(",", i.Problems);

    private static int List(Ctx ctx)
    {
        var all = InstanceStore.List();
        var json = new JsonArray(all.Select(i => (JsonNode?)Describe(i)).ToArray());
        return ctx.Render(json, all.Select(i => i.Name), w =>
        {
            if (all.Count == 0)
            {
                ctx.Notice($"no instances yet; make one with: {Dispatcher.ProgramName} instance clone");
                return;
            }
            Ctx.Table(w, ["NAME", "SLOT", "BASE", "GAME", "LEVILAMINA", "MOD", "STATUS"],
                all.Select(i => (IReadOnlyList<string>)[i.Name, i.Slot.ToString(System.Globalization.CultureInfo.InvariantCulture), i.Base,
                    i.GameVersion, i.LoaderVersion ?? "-", i.ModVersion ?? "-", Status(i)]));
        });
    }

    private static int ListBases(Ctx ctx)
    {
        var cli = LeviLauncherCli.Find(ctx.Verbose);
        var all = cli.ListInstances();
        string? picked = null;
        try
        {
            picked = BaseSelection.Select(all, null, cli.Command).Name;
        }
        catch (CliError e)
        {
            ctx.Verbose(e.Message);
        }
        var rows = all.Select(i => (Instance: i, Why: BaseSelection.Unusable(i)))
            .Where(x => x.Why is null || ctx.Bool("all"))
            .ToList();
        var json = new JsonArray(rows.Select(x => (JsonNode?)new JsonObject
        {
            ["name"] = x.Instance.Name,
            ["gameVersion"] = x.Instance.GameVersion,
            ["channel"] = x.Instance.Channel,
            ["type"] = x.Instance.Type,
            ["loaderVersion"] = x.Instance.LoaderVersion,
            ["path"] = x.Instance.Path,
            ["isDefault"] = x.Instance.IsDefault,
            ["usable"] = x.Why is null,
            ["reason"] = x.Why,
            ["selected"] = x.Instance.Name == picked,
            ["clone"] = x.Why is null ? $"{x.Instance.GameVersion}-<slot>" : null,
        }).ToArray());
        return ctx.Render(json, rows.Select(x => x.Instance.Name), w =>
        {
            if (rows.Count == 0)
            {
                ctx.Notice($"LeviLauncher ({cli.Exe}) has no instance BeDD can clone; see: {Dispatcher.ProgramName} instance list --bases --all");
                return;
            }
            Ctx.Table(w, ["", "NAME", "GAME", "TYPE", "CHANNEL", "LEVILAMINA", "NOTE"],
                rows.Select(x => (IReadOnlyList<string>)[x.Instance.Name == picked ? "*" : "", x.Instance.Name, x.Instance.GameVersion,
                    x.Instance.Type, x.Instance.Channel, x.Instance.LoaderVersion ?? "-",
                    x.Why ?? (BaseSelection.LoaderOlder(x.Instance) ? $"LeviLamina older than {BuildInfo.LeviLaminaVersion}" : "")]));
            ctx.Notice("* = the one 'instance clone' uses (--base or BEDD_INSTANCE pick another)");
        });
    }

    private static int Show(Ctx ctx)
    {
        var i = Pick(ctx);
        var json = Describe(i);
        return ctx.Render(json, [i.Name], w => WriteInstance(w, i, json));
    }

    private static void WriteInstance(TextWriter w, InstanceInfo i, JsonObject json)
    {
        w.WriteLine($"name         {i.Name}");
        w.WriteLine($"slot         {i.Slot}");
        w.WriteLine($"dir          {i.Dir}");
        w.WriteLine($"base         {i.Base} ({i.BaseDir})");
        w.WriteLine($"game         {i.GameVersion}");
        w.WriteLine($"leviLamina   {i.LoaderVersion ?? "-"}");
        w.WriteLine($"mod          {i.ModVersion ?? "-"}");
        var running = json["running"]!.AsArray();
        w.WriteLine($"running      {(running.Count == 0 ? "no" : "pid " + string.Join(", ", running))}");
        w.WriteLine($"status       {Status(i)}");
    }

    private static int Clone(Ctx ctx)
    {
        var i = InstanceStore.EnsureClone(new CloneOptions
        {
            Slot = ctx.Int("slot") ?? 1,
            Base = ctx.String("base"),
            Force = ctx.Bool("force"),
            Progress = ctx.Notice,
            Warn = ctx.Warn,
        });
        var json = Describe(i);
        return ctx.Render(json, [i.Name], w => WriteInstance(w, i, json));
    }

    private static int Refresh(Ctx ctx)
    {
        List<InstanceInfo> targets;
        bool all = ctx.Bool("all");
        if (all)
        {
            if (ctx.Arg(0) is not null || ctx.Has("slot"))
                throw CliError.Usage("--all takes no instance name or --slot");
            targets = [.. InstanceStore.List()];
        }
        else
        {
            targets = [Pick(ctx)];
        }
        var results = new JsonArray();
        var lines = new List<string>();
        foreach (var t in targets)
        {
            if (all && (t.Problems.Contains("base-missing") || InstanceStore.RunningGames(t).Count > 0))
            {
                ctx.Warn($"skipping {t.Name} ({(t.Problems.Contains("base-missing") ? "its LeviLauncher instance is gone" : "a game is running")})");
                continue;
            }
            var (after, changed) = InstanceStore.Refresh(t, ctx.Notice, ctx.Warn);
            var o = Describe(after);
            o["changed"] = changed.ToJson();
            results.Add((JsonNode)o);
            lines.Add(changed.Empty ? $"{after.Name}: up to date" : $"{after.Name}: refreshed {changed.Count} item(s)");
        }
        return ctx.Render(all ? results : results.Count > 0 ? results[0]!.DeepClone() : null, targets.Select(t => t.Name), w =>
        {
            foreach (var l in lines)
                w.WriteLine(l);
        });
    }

    private static int Remove(Ctx ctx)
    {
        var i = Pick(ctx);
        if (!ctx.Bool("yes") && !Confirm(ctx, $"delete {i.Dir} (game data of {i.Name} included)? [y/N] "))
            throw new CliError(ExitCode.Declined, "ERR_DECLINED", "not removed");
        InstanceStore.Remove(i);
        return ctx.Render(new JsonObject { ["removed"] = i.Name, ["dir"] = i.Dir }, [i.Name], w => w.WriteLine($"removed {i.Name}"));
    }

    // Asks on the console; no console means no.
    private static bool Confirm(Ctx ctx, string question)
    {
        TextReader input;
        try
        {
            input = Console.IsInputRedirected
                ? Console.In
                : new StreamReader(new FileStream("CONIN$", FileMode.Open, FileAccess.Read));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw CliError.Usage("cannot ask without a console; pass --yes");
        }
        ctx.Err.Write(question);
        ctx.Err.Flush();
        var answer = input.ReadLine();
        return answer is not null && answer.Trim().ToLowerInvariant() is "y" or "yes";
    }

    private static int PathOf(Ctx ctx)
    {
        var i = InstanceStore.Require(ctx.Arg(0));
        string kind = (ctx.Arg(1) ?? "dir").ToLowerInvariant();
        string path = kind switch
        {
            "dir" or "root" => i.Dir,
            "mods" => i.ModsDir,
            "logs" => i.LogsDir,
            "data" => i.DataDir,
            "worlds" => Worlds.WorldsDirs(i.Dir).FirstOrDefault()
                ?? Path.Combine(i.DataDir, "Users", "Shared", "games", "com.mojang", "minecraftWorlds"),
            _ => throw CliError.Usage($"'{ctx.Arg(1)}' is not dir, mods, logs, data or worlds"),
        };
        return ctx.Render(new JsonObject { ["instance"] = i.Name, ["kind"] = kind, ["path"] = path, ["exists"] = Directory.Exists(path) },
            [path], w => w.WriteLine(path));
    }
}
