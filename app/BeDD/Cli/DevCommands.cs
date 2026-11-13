using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Cli;

// "BeDD.exe dev": the developer loop. It deploys the freshly built mod (bin\BeDD) to a dev clone,
// launches it in the background and watches the desktop while it runs (Core/ForegroundWatch): every
// foreground and cursor-clip change goes to tools\out\desktop-watch.log, and a Minecraft window
// taking the foreground is flagged. The steps are the other commands ("instance clone", "mod
// install", "launch"), run in-process through the registry, so dev does exactly what a test does.
internal static class DevCommands
{
    // The commands dev is made of, in the order it runs them.
    internal static readonly string[] Steps = ["instance clone", "mod install", "launch"];

    // How long a launched game has to show up as a process before dev gives up.
    private static readonly TimeSpan StartTimeout = TimeSpan.FromMinutes(3);

    [ModuleInitializer]
    internal static void Register() => Registry.Register(new Command
    {
        Path = "dev",
        Summary = "deploy the built mod to a dev clone, launch it and watch the desktop",
        Description =
            "The developer loop: clones the base instance for --slot (\"instance clone\"), installs the mod\n" +
            "built into --from (\"mod install\"), launches the game in the background (\"launch\") and keeps\n" +
            "running. Every change of the foreground window and of the cursor clip rectangle is logged to\n" +
            "--watch-log; a line ending in \"MINECRAFT HAS THE FOREGROUND\" means a game took the focus.\n" +
            "Ctrl+C (or creating <log dir>\\dev.stop) closes the game, unless --keep, and exits; creating\n" +
            "<log dir>\\dev.restart closes the game, reinstalls the mod and launches it again.\n" +
            "With --watch-only it only watches the desktop.",
        Examples = ["dev", "dev --slot 3 --label mytest", "dev --from C:\\src\\BeDD\\bin\\BeDD --keep", "dev --watch-only"],
        Flags =
        [
            Flag.Int("slot", "", "<n>", "the slot of the dev clone", 9),
            Flag.String("from", "", "<dir>", "the built mod (default: bin\\BeDD of the checkout)"),
            Flag.String("label", "", "<label>", "the label the game shows in the manager window", "dev"),
            Flag.String("watch-log", "", "<file>", "where to log the desktop (default: tools\\out\\desktop-watch.log of the checkout)"),
            Flag.Bool("keep", "", "leave the game running when dev exits"),
            Flag.Bool("watch-only", "", "only watch the desktop: no clone, install or launch"),
        ],
        Run = Run,
    });

    private static int Run(Ctx ctx)
    {
        int slot = ctx.Int("slot") ?? 9;
        if (slot < 1)
            throw CliError.Usage("--slot must be 1 or more");
        bool watchOnly = ctx.Bool("watch-only");
        string root = FindCheckout(ctx.String("from"));
        string logFile = Path.GetFullPath(ctx.String("watch-log") ?? Path.Combine(root, "tools", "out", "desktop-watch.log"));
        string outDir = Path.GetDirectoryName(logFile)!;
        string stopFile = Path.Combine(outDir, "dev.stop");
        string restartFile = Path.Combine(outDir, "dev.restart");

        string? from = null;
        if (!watchOnly)
        {
            foreach (var step in Steps)
                Require(step);
            from = Path.GetFullPath(ctx.String("from") ?? Path.Combine(root, "bin", "BeDD"));
            if (!File.Exists(Path.Combine(from, "BeDD.dll")))
                throw CliError.NotFound("ERR_MOD_NOT_BUILT", $"there is no BeDD.dll in {from}", "xmake -P . -y");
        }

        // One dev loop per slot at a time, and one per watch log: both are taken before anything in
        // the log folder is touched, so a second dev cannot disturb a running one. Both are released
        // with their handles, however dev ends.
        using var slotLock = watchOnly ? null : TakeLock(slot);
        Directory.CreateDirectory(outDir);
        using var log = OpenLog(logFile);
        TryDelete(stopFile);
        TryDelete(restartFile);

        using var stop = new CancellationTokenSource();
        ConsoleCancelEventHandler onCancel = (_, e) =>
        {
            e.Cancel = true;
            stop.Cancel();
        };
        using var watch = new ForegroundWatch(line =>
        {
            log.WriteLine(line);
            if (line.EndsWith(ForegroundWatch.MinecraftFlag, StringComparison.Ordinal))
                ctx.Warn(line.Trim());
            else
                ctx.Verbose(line);
        });

        DevGame? game = null;
        int restarts = 0;
        string label = ctx.String("label") ?? "dev";
        Console.CancelKeyPress += onCancel;
        try
        {
            watch.Start();
            ctx.Notice($"watching the desktop: {logFile}");
            if (!watchOnly)
            {
                var (name, dir) = Clone(ctx, slot);
                game = Deploy(ctx, name, dir, from!, label);
                ctx.Notice($"{game.Instance} runs; Ctrl+C or creating {stopFile} ends dev");
            }
            else
            {
                ctx.Notice($"Ctrl+C or creating {stopFile} ends the watch");
            }

            bool seen = false;
            var launchedAt = Stopwatch.StartNew();
            while (!stop.IsCancellationRequested && !File.Exists(stopFile))
            {
                if (game is not null && File.Exists(restartFile))
                {
                    TryDelete(restartFile);
                    ctx.Notice("restart requested");
                    StopGame(ctx, game);
                    game = Deploy(ctx, game.Instance, game.Dir, from!, label);
                    restarts++;
                    seen = false;
                    launchedAt.Restart();
                }
                if (game is not null)
                {
                    if (GamesIn(game.Dir).Count > 0)
                        seen = true;
                    else if (seen)
                    {
                        ctx.Notice("the game exited");
                        break;
                    }
                    else if (launchedAt.Elapsed > StartTimeout)
                        throw CliError.Failure("ERR_GAME_NOT_STARTED",
                            $"no game of {game.Instance} appeared within {StartTimeout.TotalSeconds:0} s of the launch",
                            $"{Dispatcher.ProgramName} launch {game.Instance}");
                }
                stop.Token.WaitHandle.WaitOne(250);
            }
        }
        finally
        {
            Console.CancelKeyPress -= onCancel;
            if (game is not null && !ctx.Bool("keep"))
                StopGame(ctx, game);
            TryDelete(stopFile);
        }

        var result = new JsonObject
        {
            ["watchLog"] = logFile,
            ["minecraftForegrounds"] = watch.MinecraftForegrounds,
        };
        if (game is not null)
        {
            result["instance"] = game.Instance;
            result["dir"] = game.Dir;
            result["pid"] = game.Pid;
            result["kept"] = ctx.Bool("keep");
            result["restarts"] = restarts;
        }
        return ctx.Render(result, game is null ? [] : [game.Instance], w =>
        {
            w.WriteLine(watch.MinecraftForegrounds == 0
                ? "no Minecraft window took the foreground"
                : $"a Minecraft window took the foreground {watch.MinecraftForegrounds} time(s): see {logFile}");
        });
    }

    internal sealed record DevGame(string Instance, string Dir, int Pid);

    // ---- The steps, through the registry ----

    internal static Command Require(string path)
    {
        var (command, words) = Registry.Find(path.Split(' '));
        if (command is null || words != path.Split(' ').Length ||
            !string.Equals(command.Path, path, StringComparison.OrdinalIgnoreCase))
            throw CliError.Failure("ERR_COMMAND_MISSING",
                $"dev needs the '{path}' command, which this BeDD.exe does not have", $"{Dispatcher.ProgramName} help");
        return command;
    }

    // Runs "BeDD.exe <command> <args> --json" in-process and returns its JSON result; its progress
    // and errors go to our stderr.
    internal static JsonNode? Step(Ctx ctx, string command, params string[] args)
    {
        Require(command);
        var argv = new List<string>(command.Split(' '));
        argv.AddRange(args);
        argv.Add("--json");
        if (ctx.IsVerbose)
            argv.Add("-v");
        ctx.Verbose($"> {Dispatcher.ProgramName} {string.Join(' ', argv)}");
        var output = new StringWriter();
        int code = Dispatcher.Run([.. argv], output, ctx.Err);
        if (code != 0)
            throw new CliError((ExitCode)code, "ERR_DEV_STEP", $"'{command}' failed (exit code {code})",
                $"{Dispatcher.ProgramName} {command} {string.Join(' ', args)}".TrimEnd());
        string text = output.ToString();
        try
        {
            return string.IsNullOrWhiteSpace(text) ? null : JsonNode.Parse(text);
        }
        catch (JsonException)
        {
            return null;
        }
    }

    // The dev clone for slot, created or refreshed from the base instance.
    private static (string Name, string Dir) Clone(Ctx ctx, int slot)
    {
        var clone = Step(ctx, "instance clone", "--slot", slot.ToString(System.Globalization.CultureInfo.InvariantCulture));
        string? name = StringField(clone, "name");
        string? dir = StringField(clone, "dir");
        if (name is null || dir is null)
            throw CliError.Failure("ERR_DEV_STEP", "'instance clone' did not say which instance it made (no name and dir)");
        ctx.Notice($"dev clone: {name} ({dir})");
        return (name, dir);
    }

    // Installs the built mod into the clone and launches it.
    private static DevGame Deploy(Ctx ctx, string name, string dir, string from, string label)
    {
        if (GamesIn(dir).Count > 0)
            throw CliError.Conflict("ERR_GAME_RUNNING", $"{name} is already running", $"{Dispatcher.ProgramName} ps");

        Step(ctx, "mod install", name, "--from", from);
        ctx.Notice($"installed {from} into {name}");

        // The window that has the foreground now gets it back when the game window appears.
        Environment.SetEnvironmentVariable("BEDD_RETURN_FOCUS",
            ForegroundWatch.ForegroundHandle().ToString(System.Globalization.CultureInfo.InvariantCulture));
        var launched = Step(ctx, "launch", name, "--label", label);
        int pid = IntField(launched, "pid") ?? 0;
        ctx.Notice($"launched {name}" + (pid > 0 ? $" (pid {pid})" : ""));
        return new DevGame(name, dir, pid);
    }
    // Closes the game's window, then kills what is left of the clone's games after 10 s. Only
    // processes started from the clone's folder are touched: other games on this PC are not ours.
    private static void StopGame(Ctx ctx, DevGame game)
    {
        var pids = GamesIn(game.Dir);
        if (pids.Count == 0)
            return;
        foreach (int pid in pids)
        {
            try
            {
                using var p = Process.GetProcessById(pid);
                p.CloseMainWindow();
                if (!p.WaitForExit(10_000))
                {
                    p.Kill(entireProcessTree: true);
                    p.WaitForExit(5_000);
                }
            }
            catch (Exception e) when (e is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
            {
                // gone already
            }
        }
        ctx.Notice($"closed {game.Instance}");
    }

    // ---- Helpers ----

    // The pids of the games (Minecraft.Windows.exe) started from dir.
    internal static List<int> GamesIn(string dir)
    {
        string prefix = Path.TrimEndingDirectorySeparator(Path.GetFullPath(dir)) + Path.DirectorySeparatorChar;
        var result = new List<int>();
        foreach (var p in Process.GetProcessesByName("Minecraft.Windows"))
        {
            using (p)
            {
                string? path = ForegroundWatch.ProcessPath((uint)p.Id);
                if (path is not null && path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    result.Add(p.Id);
            }
        }
        return result;
    }

    internal static string? StringField(JsonNode? node, string name)
    {
        if (node is JsonArray { Count: > 0 } a)
            node = a[0];
        return node is JsonObject o && o[name] is JsonValue v && v.TryGetValue(out string? s) && s.Length > 0 ? s : null;
    }

    internal static int? IntField(JsonNode? node, string name)
    {
        if (node is JsonArray { Count: > 0 } a)
            node = a[0];
        return node is JsonObject o && o[name] is JsonValue v && v.TryGetValue(out int n) ? n : null;
    }

    // The checkout dev works in: the folder holding xmake.lua above the current directory, else the
    // one above BeDD.exe (bin\app\BeDD.exe), else above --from (bin\BeDD), else the current directory.
    internal static string FindCheckout(string? from, string? cwd = null, string? exeDir = null)
    {
        cwd ??= Environment.CurrentDirectory;
        exeDir ??= AppContext.BaseDirectory;
        foreach (var start in new[] { cwd, exeDir, from })
        {
            if (string.IsNullOrEmpty(start))
                continue;
            for (var d = new DirectoryInfo(Path.GetFullPath(start)); d is not null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "xmake.lua")))
                    return d.FullName;
        }
        return Path.GetFullPath(cwd);
    }

    private static FileStream TakeLock(int slot)
    {
        Directory.CreateDirectory(Paths.Locks);
        string file = Path.Combine(Paths.Locks, $"dev-{slot}.lock");
        try
        {
            var fs = new FileStream(file, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            var bytes = System.Text.Encoding.UTF8.GetBytes($"pid={Environment.ProcessId} at={DateTimeOffset.Now:o}");
            fs.SetLength(0);
            fs.Write(bytes);
            fs.Flush();
            return fs;
        }
        catch (IOException)
        {
            throw CliError.Conflict("ERR_DEV_RUNNING", $"another dev loop uses slot {slot}",
                $"{Dispatcher.ProgramName} dev --slot {slot + 1}");
        }
    }

    private static StreamWriter OpenLog(string file)
    {
        FileStream fs;
        try
        {
            // Others may read the log while dev runs, but not write it: one dev loop per log.
            fs = new FileStream(file, FileMode.OpenOrCreate, FileAccess.Write, FileShare.Read | FileShare.Delete);
        }
        catch (IOException) when (File.Exists(file))
        {
            throw CliError.Conflict("ERR_WATCH_LOG_BUSY", $"another dev loop writes {file}",
                $"{Dispatcher.ProgramName} dev --watch-log <other folder>\\desktop-watch.log");
        }
        fs.SetLength(0);
        return new StreamWriter(fs, new System.Text.UTF8Encoding(false)) { AutoFlush = true };
    }

    private static void TryDelete(string file)
    {
        try
        {
            File.Delete(file);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }
}
