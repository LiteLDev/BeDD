namespace BeDD.Core;

// A BeDD game instance: a clone of a LeviLauncher instance under Paths.Instances, named
// <base>-<slot>. Slot 1 is a clone too: BeDD never runs games in the user's LeviLauncher instance.
public sealed record InstanceInfo
{
    // Folder name under Paths.Instances, e.g. "1.26.60.05-2".
    public required string Name { get; init; }
    public required string Dir { get; init; }
    public string ModsDir => Path.Combine(Dir, "mods");
    // The LeviLauncher instance it is cloned from (its LeviLauncher name) and that one's folder.
    public required string Base { get; init; }
    public required string BaseDir { get; init; }
    public required int Slot { get; init; }
    public string GameVersion { get; init; } = "";
    public string? LoaderVersion { get; init; }
    // What it still lacks or what is wrong ("missing", "stale", "mod-missing", ...); empty when ready.
    public IReadOnlyList<string> Problems { get; init; } = [];
}

// Filled in by the instances unit (A1). The signatures are the contract other units build on.
public static class InstanceStore
{
    // Every BeDD instance under Paths.Instances.
    public static IReadOnlyList<InstanceInfo> List() =>
        throw new NotImplementedException("InstanceStore.List");

    // The instance named name (a folder under Paths.Instances), or the clone of the base instance for
    // slot when name is null. Null when it does not exist yet.
    public static InstanceInfo? Resolve(string? name, int slot = 1) =>
        throw new NotImplementedException("InstanceStore.Resolve");

    // Resolve, creating or refreshing the clone from its LeviLauncher base instance first.
    public static InstanceInfo EnsureClone(string? name, int slot = 1, Action<string>? progress = null) =>
        throw new NotImplementedException("InstanceStore.EnsureClone");
}
