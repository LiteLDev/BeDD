using System.Text.Json;
using System.Text.Json.Serialization;
using Windows.Win32;
using Windows.Win32.System.Threading;

namespace BeDD.Core;

// What a running game or server announces in run\<pid>.json while its WebSocket server listens.
// The mod writes the file and deletes it at exit; a file whose process is gone is stale.
public sealed record GameEndpoint
{
    public int Protocol { get; init; }
    // "client" (a game) or "server" (a BDS with BeDD-server).
    public string Kind { get; init; } = "client";
    public int Pid { get; init; }
    // ws://127.0.0.1:<port>/v1
    public string Url { get; init; } = "";
    // 64 hex characters; send as "Authorization: Bearer <token>" or "?token=<token>".
    public string Token { get; init; } = "";
    public string Instance { get; init; } = "";
    public string InstanceDir { get; init; } = "";
    public string Label { get; init; } = "";
    public string ModVersion { get; init; } = "";
    public DateTimeOffset? StartedAt { get; init; }

    [JsonIgnore]
    public string File { get; init; } = "";
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull)]
[JsonSerializable(typeof(GameEndpoint))]
[JsonSerializable(typeof(List<GameEndpoint>))]
internal sealed partial class DiscoveryJson : JsonSerializerContext;

public static class Discovery
{
    // The endpoints of running games and servers, oldest first. With prune, files of processes that
    // are gone are deleted.
    public static List<GameEndpoint> List(bool prune = false, string? runDir = null)
    {
        runDir ??= Paths.Run;
        var result = new List<GameEndpoint>();
        if (!Directory.Exists(runDir))
            return result;
        foreach (var file in Directory.EnumerateFiles(runDir, "*.json"))
        {
            var ep = Read(file);
            if (ep is null)
                continue;
            if (!IsAlive(ep.Pid))
            {
                if (prune)
                    TryDelete(file);
                continue;
            }
            result.Add(ep);
        }
        result.Sort((a, b) => Nullable.Compare(a.StartedAt, b.StartedAt));
        return result;
    }

    public static GameEndpoint? Read(string file)
    {
        try
        {
            using var s = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var ep = JsonSerializer.Deserialize(s, DiscoveryJson.Default.GameEndpoint);
            if (ep is null || ep.Pid <= 0 || ep.Url.Length == 0)
                return null;
            return ep with { File = file };
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            return null; // being written or deleted right now, or not ours
        }
    }

    public static bool IsAlive(int pid)
    {
        if (pid <= 0)
            return false;
        using var h = PInvoke.OpenProcess_SafeHandle(PROCESS_ACCESS_RIGHTS.PROCESS_QUERY_LIMITED_INFORMATION, false, (uint)pid);
        if (h.IsInvalid)
            return false;
        if (!PInvoke.GetExitCodeProcess(h, out uint code))
            return false;
        return code == 259; // STILL_ACTIVE
    }

    private static void TryDelete(string file)
    {
        try
        {
            File.Delete(file);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }
}
