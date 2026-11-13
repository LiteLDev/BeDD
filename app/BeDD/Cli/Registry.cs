namespace BeDD.Cli;

// Every command and noun of the command line. Command files add themselves from a
// [ModuleInitializer]; nouns are declared here, once, so that command files only add verbs. A noun
// without any registered verb is hidden from help.
public static class Registry
{
    private static readonly object Gate = new();
    private static readonly List<Command> CommandList = [];
    private static readonly Dictionary<string, string> NounSummaries = new(StringComparer.OrdinalIgnoreCase)
    {
        ["instance"] = "BeDD's game instances: clones of a LeviLauncher instance, one per slot",
        ["mod"] = "the BeDD mod in an instance",
        ["world"] = "worlds of an instance",
        ["server"] = "Bedrock Dedicated Servers for the client+server mode",
        ["manager"] = "the manager window that shows every BeDD game",
        ["process"] = "running BeDD games (alias: ps)",
    };
    private static readonly Dictionary<string, string> TopAliases = new(StringComparer.OrdinalIgnoreCase)
    {
        ["ps"] = "process list",
    };

    public static void Register(Command command)
    {
        lock (Gate)
        {
            if (CommandList.Any(c => string.Equals(c.Path, command.Path, StringComparison.OrdinalIgnoreCase)))
                throw new InvalidOperationException($"command '{command.Path}' is registered twice");
            CommandList.Add(command);
        }
    }

    public static IReadOnlyList<Command> Commands
    {
        get { lock (Gate) return CommandList.ToArray(); }
    }

    public static IReadOnlyDictionary<string, string> Nouns => NounSummaries;

    // The command named by the first one or two words of args, and how many words it took.
    public static (Command? Command, int Words) Find(IReadOnlyList<string> words)
    {
        var commands = Commands;
        if (words.Count == 0)
            return (null, 0);
        string first = words[0];
        if (TopAliases.TryGetValue(first, out var target))
        {
            var aliased = commands.FirstOrDefault(c => string.Equals(c.Path, target, StringComparison.OrdinalIgnoreCase));
            if (aliased is not null)
                return (aliased, 1);
        }
        if (words.Count >= 2)
        {
            string two = first + " " + words[1];
            foreach (var c in commands)
            {
                if (string.Equals(c.Path, two, StringComparison.OrdinalIgnoreCase))
                    return (c, 2);
                if (c.Path.StartsWith(first + " ", StringComparison.OrdinalIgnoreCase) &&
                    c.Aliases.Any(a => string.Equals(first + " " + a, two, StringComparison.OrdinalIgnoreCase)))
                    return (c, 2);
            }
        }
        foreach (var c in commands)
        {
            if (string.Equals(c.Path, first, StringComparison.OrdinalIgnoreCase) ||
                (!c.Path.Contains(' ') && c.Aliases.Any(a => string.Equals(a, first, StringComparison.OrdinalIgnoreCase))))
                return (c, 1);
        }
        return (null, 0);
    }
}
