using System.Text;

namespace BeDD.Core;

// Worlds of an instance: <instance>\Minecraft Bedrock\Users\<id>\games\com.mojang\minecraftWorlds\<folder>.
public static class Worlds
{
    // The tags that make a world multiplayer and how wide their value is: TAG_Byte (1) or TAG_Int (3).
    private static readonly (string Name, byte Type)[] MultiplayerTags =
    [
        ("MultiplayerGame", 1), ("MultiplayerGameIntent", 1), ("LANBroadcast", 1), ("LANBroadcastIntent", 1),
        ("XBLBroadcastIntent", 3), ("PlatformBroadcastIntent", 3),
    ];

    // Every minecraftWorlds folder of the instance's players (Users\Shared last).
    public static List<string> WorldsDirs(string instanceDir)
    {
        string users = Path.Combine(instanceDir, "Minecraft Bedrock", "Users");
        if (!Directory.Exists(users))
            return [];
        return Directory.EnumerateDirectories(users)
            .OrderBy(d => string.Equals(Path.GetFileName(d), "Shared", StringComparison.OrdinalIgnoreCase) ? 1 : 0)
            .ThenBy(d => d, StringComparer.OrdinalIgnoreCase)
            .Select(d => Path.Combine(d, "games", "com.mojang", "minecraftWorlds"))
            .Where(Directory.Exists)
            .ToList();
    }

    // The world folders (those with a level.dat) of the instance.
    public static List<string> All(string instanceDir) =>
        WorldsDirs(instanceDir)
            .SelectMany(Directory.EnumerateDirectories)
            .Where(d => File.Exists(Path.Combine(d, "level.dat")))
            .ToList();

    // Worlds named folder (its folder name under minecraftWorlds; one per player at most).
    public static List<string> Find(string instanceDir, string folder) =>
        All(instanceDir).Where(d => string.Equals(Path.GetFileName(d), folder, StringComparison.OrdinalIgnoreCase)).ToList();

    // Worlds whose level.dat was written in the last window: a test world just restored.
    public static List<string> Recent(string instanceDir, TimeSpan window)
    {
        var since = DateTime.UtcNow - window;
        return All(instanceDir).Where(d => File.GetLastWriteTimeUtc(Path.Combine(d, "level.dat")) > since).ToList();
    }

    // Makes a world single-player: no "multiplayer game", no LAN, Xbox Live or platform broadcast
    // (level.dat and level.dat_old). Every game instance on a PC signs in to Minecraft's signaling
    // service with the same ID, and a world opened for multiplayer registers a session there: with
    // several instances joining at once they knock each other out ("An error has occurred", code
    // "Blaze"). A single-player test world never touches it. Returns the tags that were changed.
    public static List<string> SetSinglePlayer(string worldDir)
    {
        var changed = new List<string>();
        foreach (var f in new[] { "level.dat", "level.dat_old" })
        {
            string path = Path.Combine(worldDir, f);
            if (!File.Exists(path))
                continue;
            byte[] b = File.ReadAllBytes(path);
            var tags = PatchLevelDat(b);
            if (tags.Count == 0)
                continue;
            FileLinks.ReplaceFile(path, b);
            foreach (var t in tags)
                if (!changed.Contains(t))
                    changed.Add(t);
        }
        return changed;
    }

    // Zeroes the value of each multiplayer tag in a Bedrock level.dat (an 8-byte header, then
    // little-endian NBT): the first "type, name length (LE16), name" match after the header. Returns
    // the tags whose value was not zero already.
    public static List<string> PatchLevelDat(byte[] b)
    {
        var changed = new List<string>();
        foreach (var (name, type) in MultiplayerTags)
        {
            byte[] nb = Encoding.UTF8.GetBytes(name);
            var pattern = new byte[3 + nb.Length];
            pattern[0] = type;
            pattern[1] = (byte)(nb.Length & 0xFF);
            pattern[2] = (byte)(nb.Length >> 8);
            nb.CopyTo(pattern, 3);
            int width = type == 1 ? 1 : 4;
            int at = b.AsSpan(Math.Min(8, b.Length)).IndexOf(pattern);
            if (at < 0)
                continue;
            int v = 8 + at + pattern.Length;
            if (v + width > b.Length)
                continue;
            bool nonZero = false;
            for (int k = 0; k < width; k++)
            {
                nonZero |= b[v + k] != 0;
                b[v + k] = 0;
            }
            if (nonZero)
                changed.Add(name);
        }
        return changed;
    }
}
