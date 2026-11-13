using System.Globalization;
using BeDD.Core;

namespace BeDD.Gui;

// The manager's log: <BeDD home>\logs\manager.log, started afresh once it passes 1 MiB.
internal static class ManagerLog
{
    private static readonly object Gate = new();

    public static string File => Path.Combine(Paths.Logs, "manager.log");

    public static void Trim()
    {
        try
        {
            var f = new FileInfo(File);
            if (f.Exists && f.Length > (1 << 20))
                f.Delete();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }

    public static void Write(string message)
    {
        try
        {
            lock (Gate)
            {
                Directory.CreateDirectory(Paths.Logs);
                System.IO.File.AppendAllText(File,
                    DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture) +
                    " [" + Environment.ProcessId.ToString(CultureInfo.InvariantCulture) + "] " + message + Environment.NewLine);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }
}
