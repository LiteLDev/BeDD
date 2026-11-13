using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Cli;

// world singleplayer: the level.dat edit that keeps parallel games from knocking each other out.
internal static class WorldCommands
{
    [ModuleInitializer]
    internal static void Register() => Registry.Register(new Command
    {
        Path = "world singleplayer",
        Summary = "make worlds of an instance single-player",
        Description = "Clear the multiplayer and broadcast settings in level.dat (and level.dat_old) of a world. Every game on a PC\n" +
            "signs in to Minecraft's signaling service with the same ID, and a world opened for multiplayer registers a\n" +
            "session there: several games doing so at once knock each other out (\"Blaze\"). A single-player world never does.\n" +
            "Without a world folder name, every world of the instance; --recent: those written in the last --minutes.\n" +
            "Do it while the world is not open.",
        Args = "<instance> [<world folder>]",
        MinArgs = 1,
        MaxArgs = 2,
        Flags =
        [
            Flag.Bool("recent", "", "only the worlds whose level.dat was written recently (a test world just restored)"),
            Flag.Int("minutes", "", "<n>", "what --recent means", 15),
        ],
        Examples = ["world singleplayer 1.26.60.05-2 Showcase", "world singleplayer 1.26.60.05-2 --recent"],
        Run = SinglePlayer,
    });

    private static int SinglePlayer(Ctx ctx)
    {
        var i = InstanceStore.Require(ctx.Arg(0));
        var world = ctx.Arg(1);
        bool recent = ctx.Bool("recent");
        if (world is not null && recent)
            throw CliError.Usage("give a world folder or --recent, not both");
        List<string> worlds;
        if (world is not null)
        {
            worlds = Worlds.Find(i.Dir, world);
            if (worlds.Count == 0)
                throw CliError.NotFound("ERR_WORLD_NOT_FOUND", $"{i.Name} has no world folder '{world}'",
                    $"{Dispatcher.ProgramName} instance path {i.Name} worlds");
        }
        else if (recent)
        {
            int minutes = ctx.Int("minutes") ?? 15;
            if (minutes <= 0)
                throw CliError.Usage("--minutes must be 1 or more");
            worlds = Worlds.Recent(i.Dir, TimeSpan.FromMinutes(minutes));
        }
        else
        {
            worlds = Worlds.All(i.Dir);
        }
        var results = new JsonArray();
        var lines = new List<string>();
        using (SetupLock.Acquire())
        {
            foreach (var w in worlds)
            {
                var changed = Worlds.SetSinglePlayer(w);
                results.Add((JsonNode)new JsonObject
                {
                    ["world"] = Path.GetFileName(w),
                    ["dir"] = w,
                    ["changed"] = new JsonArray(changed.Select(c => (JsonNode?)c).ToArray()),
                });
                lines.Add(changed.Count == 0 ? $"{Path.GetFileName(w)}: already single-player" : $"{Path.GetFileName(w)}: cleared {string.Join(", ", changed)}");
            }
        }
        if (worlds.Count == 0)
            ctx.Notice($"{i.Name} has no {(recent ? "recently written " : "")}worlds");
        return ctx.Render(results, worlds.Select(Path.GetFileName).OfType<string>(), w =>
        {
            foreach (var l in lines)
                w.WriteLine(l);
        });
    }
}
