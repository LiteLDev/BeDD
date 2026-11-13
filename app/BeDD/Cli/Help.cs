namespace BeDD.Cli;

internal static class Help
{
    public static void Overview(TextWriter w)
    {
        w.WriteLine($"Be.D.D. {BuildInfo.Version}: background Minecraft games for end-to-end tests of LeviLamina mods.");
        w.WriteLine();
        w.WriteLine($"Usage: {Dispatcher.ProgramName} <noun> <verb> [arguments] [flags]");
        w.WriteLine($"       {Dispatcher.ProgramName}            (no arguments) opens the manager window");
        w.WriteLine();
        var commands = Registry.Commands;
        var nouns = Registry.Nouns.Where(n => commands.Any(c => c.Path.StartsWith(n.Key + " ", StringComparison.OrdinalIgnoreCase))).ToList();
        if (nouns.Count > 0)
        {
            w.WriteLine("Nouns:");
            Ctx.Table(w, ["", ""], nouns.Select(n => (IReadOnlyList<string>)[" " + n.Key, n.Value]).Skip(0));
            w.WriteLine();
        }
        var top = commands.Where(c => !c.Path.Contains(' ')).OrderBy(c => c.Path, StringComparer.Ordinal).ToList();
        if (top.Count > 0)
        {
            w.WriteLine("Commands:");
            Ctx.Table(w, ["", ""], top.Select(c => (IReadOnlyList<string>)[" " + c.Path, c.Summary]));
            w.WriteLine();
        }
        w.WriteLine("Global flags:");
        Ctx.Table(w, ["", ""], Dispatcher.Globals.Select(FlagRow));
        w.WriteLine();
        w.WriteLine($"Run '{Dispatcher.ProgramName} help <noun>' or '{Dispatcher.ProgramName} <noun> <verb> --help' for details.");
    }

    public static void Noun(TextWriter w, string noun)
    {
        w.WriteLine($"{noun}: {Registry.Nouns[noun]}");
        w.WriteLine();
        w.WriteLine("Verbs:");
        var verbs = Registry.Commands.Where(c => c.Path.StartsWith(noun + " ", StringComparison.OrdinalIgnoreCase))
            .OrderBy(c => c.Path, StringComparer.Ordinal);
        Ctx.Table(w, ["", ""], verbs.Select(c => (IReadOnlyList<string>)[" " + c.Path[(noun.Length + 1)..], c.Summary]));
    }

    public static void Command(TextWriter w, Command c)
    {
        w.WriteLine($"Usage: {Dispatcher.ProgramName} {c.Path}{(c.Args.Length > 0 ? " " + c.Args : "")} [flags]");
        w.WriteLine();
        w.WriteLine(c.Description ?? c.Summary);
        if (c.Flags.Length > 0)
        {
            w.WriteLine();
            w.WriteLine("Flags:");
            Ctx.Table(w, ["", ""], c.Flags.Select(FlagRow));
        }
        if (c.Examples.Length > 0)
        {
            w.WriteLine();
            w.WriteLine("Examples:");
            foreach (var e in c.Examples)
                w.WriteLine($"  {Dispatcher.ProgramName} {e}");
        }
    }

    private static IReadOnlyList<string> FlagRow(Flag f)
    {
        string names = (f.Short.Length > 0 ? $"-{f.Short}, " : "    ") + "--" + f.Name + (f.ValueName is null ? "" : " " + f.ValueName);
        string help = f.Help + (f.Default is null ? "" : $" (default {f.Default})");
        return [" " + names, help];
    }
}
