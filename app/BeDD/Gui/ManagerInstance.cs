using System.Security.Cryptography;
using System.Text;
using BeDD.Core;
using Windows.Win32;

namespace BeDD.Gui;

// How the one manager of a logon session is found from outside: the named mutex it holds while it
// runs (`Local\BeDD.v2.Manager`), the named event that stops it (`Local\BeDD.v2.Manager.Stop`) and
// its window class. The names differ from v1's PowerShell manager (`Local\BeDD.Manager`), which
// links games over a pipe v2 games no longer have: one left running must not pass for a v2 manager. With BEDD_HOME pointing elsewhere every name gets a suffix made from that folder, so
// a BeDD tree of its own (tests, a recording) has a manager of its own that sees only its games.
public static class ManagerInstance
{
    private const string MutexBase = @"Local\BeDD.v2.Manager";
    private const string StopBase = @"Local\BeDD.v2.Manager.Stop";
    private const string ClassBase = "BeDD.v2.Manager";

    // Tests set this to get names nobody else uses.
    internal static string? SuffixOverride { get; set; }

    // "" for the default home, else "." and 16 hex digits of the home folder's hash.
    public static string Suffix
    {
        get
        {
            if (SuffixOverride is not null)
                return SuffixOverride;
            string root = Paths.Root;
            string def = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BeDD");
            if (string.Equals(Path.TrimEndingDirectorySeparator(root), Path.TrimEndingDirectorySeparator(def), StringComparison.OrdinalIgnoreCase))
                return "";
            byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes(Path.TrimEndingDirectorySeparator(root).ToUpperInvariant()));
            return "." + Convert.ToHexStringLower(hash, 0, 8);
        }
    }

    // Held by the running manager; launchers check it to decide whether to start BeDD.exe.
    public static string MutexName => MutexBase + Suffix;

    // Set it and the manager closes every game without asking, then exits.
    public static string StopEventName => StopBase + Suffix;

    public static string WindowClass => ClassBase + Suffix;

    // Whether a manager holds the mutex right now.
    public static bool IsRunning()
    {
        if (!Mutex.TryOpenExisting(MutexName, out var mutex))
            return false;
        using (mutex)
        {
            bool owned;
            try
            {
                owned = mutex.WaitOne(0);
            }
            catch (AbandonedMutexException)
            {
                owned = true;
            }
            if (owned)
                mutex.ReleaseMutex();
            return !owned;
        }
    }

    // The manager's window and process id, or (0, 0) when it has no window (yet).
    public static (nint Hwnd, int Pid, bool Visible) FindWindow()
    {
        var hwnd = PInvoke.FindWindow(WindowClass, null);
        if (hwnd.IsNull)
            return (0, 0, false);
        uint pid;
        unsafe
        {
            _ = PInvoke.GetWindowThreadProcessId(hwnd, &pid);
        }
        return (hwnd, (int)pid, PInvoke.IsWindowVisible(hwnd));
    }

    // The games a manager would show: the running clients in the run folder.
    public static int CountGames() =>
        Discovery.List().Count(e => string.Equals(e.Kind, "client", StringComparison.OrdinalIgnoreCase));

    // Asks the running manager to close its games and exit. False when none runs.
    public static bool SignalStop()
    {
        if (!EventWaitHandle.TryOpenExisting(StopEventName, out var ev))
            return false;
        using (ev)
            return ev.Set();
    }

    // Waits until no manager holds the mutex. False on timeout.
    public static bool WaitForExit(TimeSpan timeout)
    {
        if (!Mutex.TryOpenExisting(MutexName, out var mutex))
            return true;
        using (mutex)
        {
            try
            {
                if (!mutex.WaitOne(timeout))
                    return false;
            }
            catch (AbandonedMutexException)
            {
            }
            mutex.ReleaseMutex();
            return true;
        }
    }
}
