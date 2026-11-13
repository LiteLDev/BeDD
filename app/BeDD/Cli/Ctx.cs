using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization.Metadata;

namespace BeDD.Cli;

// What a command's Run gets: its positional arguments, its flags, the global switches and the
// output helpers.
public sealed class Ctx
{
    private static readonly JsonWriterOptions WriterOptions = new() { Indented = true };

    private readonly Dictionary<string, List<string>> _flags;

    internal Ctx(Command command, IReadOnlyList<string> args, Dictionary<string, List<string>> flags,
        bool json, bool quiet, bool verbose, TextWriter stdout, TextWriter stderr)
    {
        Command = command;
        Args = args;
        _flags = flags;
        JsonOutput = json;
        Quiet = quiet;
        IsVerbose = verbose;
        Out = stdout;
        Err = stderr;
    }

    public Command Command { get; }
    public IReadOnlyList<string> Args { get; }
    public bool JsonOutput { get; }
    public bool Quiet { get; }
    public bool IsVerbose { get; }
    public TextWriter Out { get; }
    public TextWriter Err { get; }

    public string? Arg(int index) => index < Args.Count ? Args[index] : null;

    public bool Has(string flag) => _flags.ContainsKey(flag);

    public bool Bool(string flag) => _flags.TryGetValue(flag, out var v) && v.Count > 0 && v[^1] == "true";

    public string? String(string flag)
    {
        if (_flags.TryGetValue(flag, out var v) && v.Count > 0)
            return v[^1];
        return Command.Flags.FirstOrDefault(f => f.Name == flag)?.Default;
    }

    public int? Int(string flag)
    {
        var s = String(flag);
        if (s is null)
            return null;
        if (!int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out int n))
            throw CliError.Usage($"--{flag} must be a whole number, not '{s}'");
        return n;
    }

    public IReadOnlyList<string> Strings(string flag) => _flags.TryGetValue(flag, out var v) ? v : [];

    // Progress and remarks for a person; silent with -q and --json.
    public void Notice(string message)
    {
        if (!Quiet && !JsonOutput)
            Err.WriteLine(message);
    }

    public void Warn(string message) => Err.WriteLine("warning: " + message);

    public void Verbose(string message)
    {
        if (IsVerbose)
            Err.WriteLine(message);
    }

    // The result: the JSON document with --json, the identifiers with -q, else what text writes.
    public int Render(JsonNode? json, IEnumerable<string> ids, Action<TextWriter>? text)
    {
        if (JsonOutput)
            Json(json);
        else if (Quiet)
            foreach (var id in ids)
                Out.WriteLine(id);
        else
            text?.Invoke(Out);
        return (int)ExitCode.Ok;
    }

    public int Render<T>(T value, JsonTypeInfo<T> type, IEnumerable<string> ids, Action<TextWriter>? text)
    {
        if (JsonOutput)
        {
            Json(value, type);
            return (int)ExitCode.Ok;
        }
        return Render(null, ids, text);
    }

    public void Json(JsonNode? node)
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms, WriterOptions))
        {
            if (node is null)
                w.WriteNullValue();
            else
                node.WriteTo(w);
        }
        Out.WriteLine(System.Text.Encoding.UTF8.GetString(ms.ToArray()));
    }

    public void Json<T>(T value, JsonTypeInfo<T> type)
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms, WriterOptions))
            JsonSerializer.Serialize(w, value, type);
        Out.WriteLine(System.Text.Encoding.UTF8.GetString(ms.ToArray()));
    }

    // Columns separated by two spaces, the header in upper case.
    public static void Table(TextWriter w, IReadOnlyList<string> header, IEnumerable<IReadOnlyList<string>> rows)
    {
        var all = new List<IReadOnlyList<string>> { header };
        all.AddRange(rows);
        var widths = new int[header.Count];
        foreach (var r in all)
            for (int i = 0; i < r.Count && i < widths.Length; i++)
                widths[i] = Math.Max(widths[i], r[i].Length);
        foreach (var r in all)
        {
            var cells = new string[widths.Length];
            for (int i = 0; i < widths.Length; i++)
            {
                string c = i < r.Count ? r[i] : "";
                cells[i] = i == widths.Length - 1 ? c : c.PadRight(widths[i]);
            }
            w.WriteLine(string.Join("  ", cells).TrimEnd());
        }
    }
}
