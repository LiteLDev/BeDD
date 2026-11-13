using System.Text.Json.Nodes;

namespace BeDD.Cli;

public static class Dispatcher
{
    public const string ProgramName = "BeDD.exe";

    private static readonly Flag[] GlobalFlags =
    [
        Flag.Bool("json", "", "print the result as one JSON document"),
        Flag.Bool("quiet", "q", "print identifiers only; no progress"),
        Flag.Bool("verbose", "v", "print details of what happens to stderr"),
        Flag.Bool("help", "h", "show help"),
    ];

    public static int Run(string[] argv, TextWriter stdout, TextWriter stderr)
    {
        bool json = argv.Contains("--json");
        try
        {
            return RunCore(argv, stdout, stderr);
        }
        catch (CliError e)
        {
            Report(e.Exit, e.Code, e.Message, e.Hint, json, stdout, stderr);
            return (int)e.Exit;
        }
        catch (OperationCanceledException)
        {
            Report(ExitCode.Failure, "ERR_CANCELLED", "cancelled", null, json, stdout, stderr);
            return (int)ExitCode.Failure;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        {
            Report(ExitCode.Failure, "ERR_IO", e.Message, null, json, stdout, stderr);
            return (int)ExitCode.Failure;
        }
    }

    private static void Report(ExitCode exit, string code, string message, string? hint, bool json, TextWriter stdout, TextWriter stderr)
    {
        if (json)
        {
            var err = new JsonObject { ["code"] = code, ["message"] = message, ["exit"] = (int)exit };
            if (hint is not null)
                err["hint"] = hint;
            stdout.WriteLine(new JsonObject { ["error"] = err }.ToJsonString());
        }
        stderr.WriteLine("error: " + message);
        if (hint is not null)
            stderr.WriteLine("hint: " + hint);
    }

    private static int RunCore(string[] argv, TextWriter stdout, TextWriter stderr)
    {
        var words = new List<string>();
        foreach (var a in argv)
        {
            if (a.StartsWith('-'))
                break;
            words.Add(a);
        }
        if (words.Count == 0)
        {
            if (argv.Contains("--help") || argv.Contains("-h"))
            {
                Help.Overview(stdout);
                return 0;
            }
            throw CliError.Usage("a command is needed", $"{ProgramName} help");
        }
        if (words[0] == "help")
        {
            words.RemoveAt(0);
            return HelpFor(words, stdout);
        }

        var (command, used) = Registry.Find(words);
        if (command is null)
        {
            if (Registry.Nouns.ContainsKey(words[0]) && (argv.Contains("--help") || argv.Contains("-h") || words.Count == 1))
            {
                Help.Noun(stdout, words[0]);
                return words.Count == 1 && !(argv.Contains("--help") || argv.Contains("-h")) ? (int)ExitCode.Usage : 0;
            }
            throw CliError.Usage($"unknown command '{string.Join(' ', words.Take(2))}'", $"{ProgramName} help");
        }

        var rest = argv.Skip(used).ToList();
        var (positional, flags) = Parse(command, rest);
        if (flags.ContainsKey("help"))
        {
            Help.Command(stdout, command);
            return 0;
        }
        if (positional.Count < command.MinArgs)
            throw CliError.Usage($"{command.Path} needs {(command.Args.Length > 0 ? command.Args : $"{command.MinArgs} argument(s)")}",
                $"{ProgramName} {command.Path} --help");
        if (positional.Count > command.MaxArgs)
            throw CliError.Usage($"too many arguments for {command.Path}", $"{ProgramName} {command.Path} --help");

        var ctx = new Ctx(command, positional, flags,
            flags.ContainsKey("json"), flags.ContainsKey("quiet"), flags.ContainsKey("verbose"), stdout, stderr);
        return command.Run(ctx);
    }

    private static int HelpFor(List<string> words, TextWriter stdout)
    {
        if (words.Count == 0)
        {
            Help.Overview(stdout);
            return 0;
        }
        var (command, _) = Registry.Find(words);
        if (command is not null)
        {
            Help.Command(stdout, command);
            return 0;
        }
        if (Registry.Nouns.ContainsKey(words[0]))
        {
            Help.Noun(stdout, words[0]);
            return 0;
        }
        throw CliError.Usage($"unknown command '{string.Join(' ', words)}'", $"{ProgramName} help");
    }

    // Flags may come anywhere after the command words: --name value, --name=value, -n value, and
    // --name / --no-name for switches. "--" ends the flags.
    internal static (List<string> Positional, Dictionary<string, List<string>> Flags) Parse(Command command, List<string> args)
    {
        var all = command.Flags.Concat(GlobalFlags).ToArray();
        var positional = new List<string>();
        var flags = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        for (int i = 0; i < args.Count; i++)
        {
            string a = args[i];
            if (a == "--")
            {
                positional.AddRange(args.Skip(i + 1));
                break;
            }
            if (!a.StartsWith('-') || a == "-")
            {
                positional.Add(a);
                continue;
            }
            string name = a.TrimStart('-');
            string? value = null;
            int eq = name.IndexOf('=');
            if (eq >= 0)
            {
                value = name[(eq + 1)..];
                name = name[..eq];
            }
            bool negated = false;
            Flag? flag = a.StartsWith("--")
                ? all.FirstOrDefault(f => f.Name == name)
                : all.FirstOrDefault(f => f.Short.Length > 0 && f.Short == name);
            if (flag is null && a.StartsWith("--no-"))
            {
                flag = all.FirstOrDefault(f => f.Kind == FlagKind.Bool && f.Name == name[3..]);
                negated = flag is not null;
            }
            if (flag is null)
                throw CliError.Usage($"unknown flag '{a.Split('=')[0]}'", $"{ProgramName} {command.Path} --help");
            if (flag.Kind == FlagKind.Bool)
            {
                if (value is not null && value is not ("true" or "false"))
                    throw CliError.Usage($"--{flag.Name} takes no value");
                bool on = !negated && value != "false";
                Add(flags, flag.Name, on ? "true" : "false");
                continue;
            }
            if (value is null)
            {
                if (i + 1 >= args.Count)
                    throw CliError.Usage($"--{flag.Name} needs a value");
                value = args[++i];
            }
            if (flag.Kind == FlagKind.Int && !int.TryParse(value, System.Globalization.NumberStyles.Integer,
                    System.Globalization.CultureInfo.InvariantCulture, out _))
                throw CliError.Usage($"--{flag.Name} must be a whole number, not '{value}'");
            if (flag.Kind != FlagKind.Strings)
                flags.Remove(flag.Name);
            Add(flags, flag.Name, value);
        }
        // A global switch turned off (--no-json) counts as absent; a command's own switch keeps
        // "false" so the command can tell --no-x from no flag at all (Ctx.Has).
        foreach (var g in GlobalFlags)
            if (flags.TryGetValue(g.Name, out var v) && v[^1] == "false")
                flags.Remove(g.Name);
        return (positional, flags);
    }

    private static void Add(Dictionary<string, List<string>> flags, string name, string value)
    {
        if (!flags.TryGetValue(name, out var list))
            flags[name] = list = [];
        list.Add(value);
    }

    internal static IReadOnlyList<Flag> Globals => GlobalFlags;
}
