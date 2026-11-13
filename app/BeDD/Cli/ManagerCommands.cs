using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using BeDD.Core;
using BeDD.Gui;

namespace BeDD.Cli;

// manager status / manager stop: the manager window seen from the command line. The manager itself
// starts with `BeDD.exe` and no arguments.
internal static class ManagerCommands
{
    [ModuleInitializer]
    internal static void Register()
    {
        Registry.Register(new Command
        {
            Path = "manager status",
            Summary = "tell whether the manager window runs",
            Description = "Exit code 0 either way; the result says whether it runs and its process id.",
            Examples = ["manager status", "manager status --json"],
            Run = Status,
        });
        Registry.Register(new Command
        {
            Path = "manager stop",
            Summary = "close the manager window and every game it shows",
            Description = "Closes every game without asking (each gets {op:\"close\"}, and is killed after 10 s), " +
                          "then waits until the manager has exited.",
            Flags =
            [
                Flag.Bool("yes", "y", "do not ask for confirmation"),
                Flag.Int("timeout", "", "seconds", "how long to wait for the manager to exit", 60),
            ],
            Examples = ["manager stop --yes"],
            Run = Stop,
        });
    }

    private static JsonObject Describe()
    {
        bool running = ManagerInstance.IsRunning();
        var (_, pid, visible) = running ? ManagerInstance.FindWindow() : (0, 0, false);
        var o = new JsonObject { ["running"] = running };
        if (running)
        {
            o["pid"] = pid > 0 ? pid : null;
            o["stopping"] = pid > 0 && !visible;
        }
        o["games"] = ManagerInstance.CountGames();
        o["mutex"] = ManagerInstance.MutexName;
        o["log"] = ManagerLog.File;
        return o;
    }

    private static int Status(Ctx ctx)
    {
        var o = Describe();
        bool running = (bool)o["running"]!;
        string pid = o["pid"] is JsonValue p ? p.ToString() : "";
        return ctx.Render(o, running && pid.Length > 0 ? [pid] : [], w =>
        {
            if (!running)
            {
                w.WriteLine("the manager is not running");
                return;
            }
            string state = o["stopping"] is JsonValue s && (bool)s ? "stopping" : "running";
            w.WriteLine(pid.Length > 0 ? $"the manager is {state} (pid {pid})" : "the manager is starting");
            w.WriteLine("games: " + ((int)o["games"]!).ToString(CultureInfo.InvariantCulture));
        });
    }

    private static int Stop(Ctx ctx)
    {
        int timeout = ctx.Int("timeout") ?? 60;
        if (timeout <= 0)
            throw CliError.Usage("--timeout must be at least 1");
        if (!ManagerInstance.IsRunning())
        {
            ctx.Notice("the manager is not running");
            return ctx.Render(new JsonObject { ["stopped"] = false, ["running"] = false }, [], null);
        }
        if (!ctx.Bool("yes"))
        {
            int games = ManagerInstance.CountGames();
            string what = games == 1 ? "the game it shows" : $"the {games} games it shows";
            if (Console.IsInputRedirected || ctx.JsonOutput)
                throw new CliError(ExitCode.Declined, "ERR_CONFIRMATION_REQUIRED",
                    $"stopping the manager also closes {what}; confirm with --yes", "BeDD.exe manager stop --yes");
            ctx.Err.Write($"stopping the manager also closes {what}. continue? [y/N] ");
            string? answer = Console.ReadLine();
            if (!string.Equals(answer?.Trim(), "y", StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(answer?.Trim(), "yes", StringComparison.OrdinalIgnoreCase))
                throw new CliError(ExitCode.Declined, "ERR_DECLINED", "not stopped");
        }
        if (!ManagerInstance.SignalStop())
        {
            // It exited in the meantime, or it is still starting and has no stop event yet.
            if (!ManagerInstance.IsRunning())
                return ctx.Render(new JsonObject { ["stopped"] = false, ["running"] = false }, [], null);
            throw CliError.Failure("ERR_MANAGER_STOP", "the manager has no stop event yet; try again in a moment",
                "BeDD.exe manager stop --yes");
        }
        ctx.Notice("waiting for the manager to close its games and exit");
        if (!ManagerInstance.WaitForExit(TimeSpan.FromSeconds(timeout)))
            throw CliError.Failure("ERR_MANAGER_STOP", $"the manager did not exit within {timeout} s",
                "BeDD.exe manager status");
        return ctx.Render(new JsonObject { ["stopped"] = true, ["running"] = false }, [], w => w.WriteLine("the manager stopped"));
    }
}
