namespace BeDD.Core;

// BeDD's own folders. Everything lives under %LOCALAPPDATA%\BeDD (machine-local, never roaming: a
// clone holds its own game data). BEDD_HOME moves the whole tree; BEDD_INSTANCES_DIR and
// BEDD_SERVERS_DIR move one part (e.g. instances onto the volume of the LeviLauncher instance they
// are hardlinked from). LeviLauncher's folders are never written.
public static class Paths
{
    public static string Root => Env("BEDD_HOME") ?? Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BeDD");

    // Game instances: <base>-<slot> clones of a LeviLauncher instance.
    public static string Instances => Env("BEDD_INSTANCES_DIR") ?? Path.Combine(Root, "instances");

    // Discovery files the mod writes while its WebSocket server listens: run\<pid>.json.
    public static string Run => Path.Combine(Root, "run");

    // Machine-wide locks (signaling, slots), held through open file handles.
    public static string Locks => Path.Combine(Root, "locks");

    // Bedrock Dedicated Servers for the client+server mode.
    public static string Servers => Env("BEDD_SERVERS_DIR") ?? Path.Combine(Root, "servers");

    // Downloaded tools (lip for servers, LeviLauncher when none is installed).
    public static string Tools => Path.Combine(Root, "tools");

    // The app's own logs (manager.log, ...).
    public static string Logs => Path.Combine(Root, "logs");

    private static string? Env(string name)
    {
        var v = Environment.GetEnvironmentVariable(name);
        return string.IsNullOrWhiteSpace(v) ? null : Path.GetFullPath(v.Trim());
    }
}
