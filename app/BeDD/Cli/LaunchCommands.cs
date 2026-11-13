using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Cli;

internal static class LaunchCommands
{
    [ModuleInitializer]
    internal static void Register() => Registry.Register(new Command
    {
        Path = "launch",
        Aliases = ["play"],
        Summary = "start a game of an instance in the background",
        Args = "[<instance>]",
        MaxArgs = 1,
        Description =
            "Starts Minecraft.Windows.exe of a BeDD instance without taking the foreground and waits until\n" +
            "the mod's WebSocket server listens; prints its address and token. The instance is cloned from\n" +
            "its LeviLauncher instance first when needed (by name, or the clone for --slot). The game shows\n" +
            "up in the manager window (started unless it runs) and holds the signaling lock for its whole\n" +
            "life: shared, or with --exclusive-signaling the PC to itself (for runs that join online).",
        Flags =
        [
            Flag.Int("slot", "", "<n>", "the clone of the base instance for this slot when no instance is named", 1),
            Flag.String("dir", "", "<folder>", "start the instance in this folder instead (any folder with Minecraft.Windows.exe)"),
            Flag.String("label", "", "<text>", "the game's name in the manager window (default: BEDD_LABEL, else the calling repository)"),
            Flag.Bool("exclusive-signaling", "", "have the PC to itself: wait for every other game to close, keep new ones waiting"),
            Flag.Bool("no-manager", "", "do not start the manager window"),
            Flag.Bool("detach", "", "return once the process is created; do not wait for the endpoint"),
            Flag.String("timeout", "", "<duration>", "how long to wait for the endpoint", "3m"),
            Flag.String("lock-timeout", "", "<duration>", "how long to wait for the signaling lock", "2h"),
        ],
        Examples =
        [
            "launch 1.26.60.05-2",
            "launch --slot 3 --json",
            "launch --dir D:\\games\\mc-test --label smoke --detach -q",
            "launch 1.26.60.05-1 --exclusive-signaling",
        ],
        Run = Run,
    });

    private static int Run(Ctx ctx)
    {
        string? name = ctx.Arg(0);
        string? dirFlag = ctx.String("dir");
        int slot = ctx.Int("slot") ?? 1;
        if (slot < 1)
            throw CliError.Usage("--slot must be 1 or more");
        var timeout = Launcher.ParseDuration(ctx.String("timeout") ?? "3m");
        var lockTimeout = Launcher.ParseDuration(ctx.String("lock-timeout") ?? "2h");

        string dir, instance;
        if (dirFlag is not null)
        {
            if (name is not null)
                throw CliError.Usage("name an instance or pass --dir, not both");
            dir = Path.GetFullPath(dirFlag);
            if (!Directory.Exists(dir))
                throw CliError.NotFound("ERR_INSTANCE_NOT_FOUND", $"no folder {dir}");
            instance = Path.GetFileName(dir.TrimEnd('\\', '/'));
        }
        else
        {
            InstanceInfo info = InstanceStore.EnsureClone(name, slot, ctx.Notice);
            dir = info.Dir;
            instance = info.Name;
        }

        string label = ctx.String("label") is { Length: > 0 } l ? l
            : Environment.GetEnvironmentVariable("BEDD_LABEL") is { Length: > 0 } e ? e
            : Launcher.CallerLabel(Environment.CurrentDirectory);

        var result = Launcher.Launch(new LaunchOptions
        {
            Dir = dir,
            Instance = instance,
            Label = label,
            ExclusiveSignaling = ctx.Bool("exclusive-signaling"),
            Manager = !ctx.Bool("no-manager"),
            Detach = ctx.Bool("detach"),
            Timeout = timeout,
            LockTimeout = lockTimeout,
        }, ctx.Notice);

        var ep = result.Endpoint;
        var json = new JsonObject
        {
            ["pid"] = result.Pid,
            ["instance"] = result.Instance,
            ["instanceDir"] = result.InstanceDir,
            ["url"] = ep?.Url,
            ["token"] = ep?.Token,
            ["label"] = result.Label,
        };
        return ctx.Render(json, [result.Pid.ToString(System.Globalization.CultureInfo.InvariantCulture)], w =>
        {
            w.WriteLine($"pid       {result.Pid}");
            w.WriteLine($"instance  {result.Instance}");
            w.WriteLine($"folder    {result.InstanceDir}");
            w.WriteLine($"label     {result.Label}");
            if (ep is not null)
            {
                w.WriteLine($"url       {ep.Url}");
                w.WriteLine($"token     {ep.Token}");
            }
        });
    }
}
