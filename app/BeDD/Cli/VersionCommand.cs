using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;

namespace BeDD.Cli;

internal static class VersionCommand
{
    [ModuleInitializer]
    internal static void Register() => Registry.Register(new Command
    {
        Path = "version",
        Summary = "print version information",
        Examples = ["version", "version --json"],
        Run = ctx => ctx.Render(
            new JsonObject
            {
                ["version"] = BuildInfo.Version,
                ["gameVersion"] = BuildInfo.GameVersion,
                ["leviLaminaVersion"] = BuildInfo.LeviLaminaVersion,
                ["protocol"] = BuildInfo.Protocol,
                ["home"] = Core.Paths.Root,
            },
            [BuildInfo.Version],
            w =>
            {
                w.WriteLine($"Be.D.D. {BuildInfo.Version}");
                w.WriteLine($"game {BuildInfo.GameVersion}, LeviLamina {BuildInfo.LeviLaminaVersion}, protocol {BuildInfo.Protocol}");
                w.WriteLine($"home {Core.Paths.Root}");
            }),
    });
}
