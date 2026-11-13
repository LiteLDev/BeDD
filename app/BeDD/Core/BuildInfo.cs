namespace BeDD;

// The game and LeviLamina this build of BeDD is made for. CI checks LeviLaminaVersion against the
// levilamina package in xmake.lua; keep both lines in this exact form.
public static class BuildInfo
{
    public const string GameVersion = "1.26.60";
    public const string LeviLaminaVersion = "26.60.7";
    // The WebSocket protocol the mod speaks (ping's "protocol").
    public const int Protocol = 2;

    public static string Version { get; } =
        typeof(BuildInfo).Assembly.GetName().Version?.ToString(3) ?? "0.0.0";
}
