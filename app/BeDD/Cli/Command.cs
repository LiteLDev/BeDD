namespace BeDD.Cli;

// The command line follows LeviLauncher's (internal/cli/doc.go): "BeDD.exe <noun> <verb>
// [arguments] [flags]", one file per command or small group of commands that registers itself from
// a [ModuleInitializer] and touches no other file:
//
//     internal static class InstanceCommands
//     {
//         [ModuleInitializer]
//         internal static void Register() => Registry.Register(new Command
//         {
//             Path = "instance list",
//             Summary = "list BeDD's game instances",
//             Flags = [Flag.Bool("all", "", "also list the LeviLauncher instances clones are made from")],
//             Run = ctx => { ...; return ctx.Render(node, ids, table); },
//         });
//     }
//
// Conventions:
//   - stdout carries only the result (Ctx.Render / Ctx.Json / Ctx.Print). Everything else goes to
//     stderr: Ctx.Notice (silenced by -q and --json), Ctx.Warn, Ctx.Verbose (-v only).
//   - --json prints one document: an array for lists, an object otherwise, camelCase keys, RFC 3339
//     times, sizes in bytes. -q prints identifiers only, one per line.
//   - Errors are thrown as CliError with an ExitCode; the dispatcher prints "error: <message>" and
//     the hint ("hint: <runnable command>"), or {"error":{code,message,hint}} with --json.
//   - Messages are plain lower-case English without a final period.
//   - Names match exactly, ignoring case, never by prefix.

public enum ExitCode
{
    Ok = 0,
    Failure = 1,
    Usage = 2,
    NotFound = 4,
    Conflict = 5,
    Declined = 10,
}

public sealed class CliError(ExitCode exit, string code, string message, string? hint = null) : Exception(message)
{
    public ExitCode Exit { get; } = exit;
    // A stable identifier for --json consumers, e.g. "ERR_INSTANCE_NOT_FOUND".
    public string Code { get; } = code;
    public string? Hint { get; } = hint;

    public static CliError Usage(string message, string? hint = null) => new(ExitCode.Usage, "ERR_USAGE", message, hint);
    public static CliError NotFound(string code, string message, string? hint = null) => new(ExitCode.NotFound, code, message, hint);
    public static CliError Conflict(string code, string message, string? hint = null) => new(ExitCode.Conflict, code, message, hint);
    public static CliError Failure(string code, string message, string? hint = null) => new(ExitCode.Failure, code, message, hint);
}

public enum FlagKind
{
    Bool,
    String,
    Int,
    Strings, // repeatable
}

public sealed record Flag(string Name, string Short, FlagKind Kind, string Help, string? ValueName = null, string? Default = null)
{
    public static Flag Bool(string name, string shortName, string help) => new(name, shortName, FlagKind.Bool, help);
    public static Flag String(string name, string shortName, string valueName, string help, string? def = null) =>
        new(name, shortName, FlagKind.String, help, valueName, def);
    public static Flag Int(string name, string shortName, string valueName, string help, int? def = null) =>
        new(name, shortName, FlagKind.Int, help, valueName, def?.ToString(System.Globalization.CultureInfo.InvariantCulture));
    public static Flag Strings(string name, string shortName, string valueName, string help) =>
        new(name, shortName, FlagKind.Strings, help, valueName);
}

public sealed class Command
{
    // "<noun> <verb>", or one word for a top-level command ("launch", "version").
    public required string Path { get; init; }
    public required string Summary { get; init; }
    public string Args { get; init; } = "";
    public string? Description { get; init; }
    public string[] Examples { get; init; } = [];
    public Flag[] Flags { get; init; } = [];
    public string[] Aliases { get; init; } = [];
    public int MinArgs { get; init; }
    public int MaxArgs { get; init; }
    // Returns the exit code (normally ExitCode.Ok); failures throw CliError.
    public required Func<Ctx, int> Run { get; init; }
}
