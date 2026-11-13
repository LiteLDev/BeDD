using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Cli;

// mod install|uninstall|config: the BeDD mod in an instance.
internal static class ModCommands
{
    private static readonly Flag[] SettingFlags =
    [
        Flag.Strings("set", "", "<key=value>", "a config setting (repeatable; the value is JSON when it parses, else text)"),
        Flag.String("world-mode", "", "<mode>", "where worlds run: ClientOnly, ClientServer or Remote (config \"mode\")"),
        Flag.String("server-host", "", "<host>", "with --world-mode Remote: the server the game joins"),
    ];

    [ModuleInitializer]
    internal static void Register()
    {
        Registry.Register(new Command
        {
            Path = "mod install",
            Summary = "put the BeDD mod into an instance",
            Description = "Copy BeDD.dll and manifest.json into <instance>\\mods\\BeDD, write its config\\config.json with only the\n" +
                "settings given (every other one keeps the mod's default) and switch LeviLamina to its built-in crash logger\n" +
                "(no console window as the game starts). Replaces an earlier install, its config included.\n" +
                "The files come from --from, else BEDD_MOD_DIR, else mod\\BeDD next to BeDD.exe, else bin\\BeDD of the checkout\n" +
                "the current folder is in.",
            Args = "<instance>",
            MinArgs = 1,
            MaxArgs = 1,
            Flags = [Flag.String("from", "", "<dir>", "the folder with BeDD.dll and manifest.json"), .. SettingFlags],
            Examples =
            [
                "mod install 1.26.60.05-2",
                "mod install 1.26.60.05-2 --from bin\\BeDD --set manager=false --set timeScale=2",
                "mod install 1.26.60.05-2 --world-mode Remote --server-host play.example.net",
            ],
            Run = Install,
        });
        Registry.Register(new Command
        {
            Path = "mod uninstall",
            Summary = "take the BeDD mod out of an instance",
            Args = "<instance>",
            MinArgs = 1,
            MaxArgs = 1,
            Examples = ["mod uninstall 1.26.60.05-2"],
            Run = Uninstall,
        });
        Registry.Register(new Command
        {
            Path = "mod config",
            Summary = "show or change the BeDD mod's config in an instance",
            Description = "Print mods\\BeDD\\config\\config.json of an instance after applying --set, --unset and --world-mode.\n" +
                "The game reads it when it starts.",
            Args = "<instance>",
            MinArgs = 1,
            MaxArgs = 1,
            Flags = [.. SettingFlags, Flag.Strings("unset", "", "<key>", "remove a setting (back to the mod's default; repeatable)")],
            Examples = ["mod config 1.26.60.05-2", "mod config 1.26.60.05-2 --set trace=true --unset timeScale"],
            Run = Config,
        });
    }

    private static ModSettings Settings(Ctx ctx) => new()
    {
        Set = ctx.Strings("set"),
        Unset = ctx.Command.Flags.Any(f => f.Name == "unset") ? ctx.Strings("unset") : [],
        WorldMode = ctx.String("world-mode"),
        ServerHost = ctx.String("server-host"),
    };

    private static int Install(Ctx ctx)
    {
        var i = InstanceStore.Require(ctx.Arg(0));
        var settings = Settings(ctx);
        // Bad settings fail before anything is copied.
        ModDeploy.Apply(new JsonObject(), settings);
        string source = ModDeploy.FindSource(ctx.String("from"));
        ctx.Verbose($"installing the mod from {source}");
        var result = ModDeploy.Install(i, source, settings, ctx.Warn);
        if ((string?)result["crashLogger"] == "noConfig")
            ctx.Notice("LeviLamina has no config yet: its crash logger is switched to built-in by the next 'instance refresh' (or clone) after the first game start");
        return ctx.Render(result, [i.Name], w =>
        {
            w.WriteLine($"installed BeDD {(string?)result["version"] ?? "?"} into {i.Name} ({result["dir"]})");
            var config = result["config"]!.AsObject();
            if (config.Count > 0)
                w.WriteLine($"config {config.ToJsonString()}");
        });
    }

    private static int Uninstall(Ctx ctx)
    {
        var i = InstanceStore.Require(ctx.Arg(0));
        bool removed = ModDeploy.Uninstall(i);
        return ctx.Render(new JsonObject { ["instance"] = i.Name, ["removed"] = removed }, [i.Name],
            w => w.WriteLine(removed ? $"removed BeDD from {i.Name}" : $"{i.Name} had no BeDD mod"));
    }

    private static int Config(Ctx ctx)
    {
        var i = InstanceStore.Require(ctx.Arg(0));
        var config = ModDeploy.Configure(i, Settings(ctx));
        return ctx.Render(config, [i.Name], w => w.WriteLine(System.Text.Encoding.UTF8.GetString(JsonText.Indented(config))));
    }
}
