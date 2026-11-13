using System.Diagnostics;
using Windows.Win32;
using Windows.Win32.UI.WindowsAndMessaging;

namespace BeDD;

// BeDD.exe is a console program, so shells and agents wait for the command line and get its output
// and exit code (a GUI-subsystem exe returns at once in PowerShell and cmd). Without arguments it is
// the manager window, which must not keep a console around:
//   - double-clicked in Explorer, Windows made a console just for it: hide it and let it go (a
//     console window may flash for a moment);
//   - started without arguments from a terminal: start a copy of itself without a console and
//     return at once, so the terminal stays free;
//   - started detached (launch starts the manager this way): no console at all, nothing to do.
internal static class ConsoleSetup
{
    // True when this process should go on as the manager window; false when it handed that to a
    // copy of itself and should exit.
    public static bool PrepareForGui()
    {
        var console = PInvoke.GetConsoleWindow();
        if (console == default)
            return true;
        if (ConsoleProcessCount() <= 1)
        {
            PInvoke.ShowWindow(console, SHOW_WINDOW_CMD.SW_HIDE);
            PInvoke.FreeConsole();
            return true;
        }
        string? self = Environment.ProcessPath;
        if (self is null)
            return true;
        var psi = new ProcessStartInfo(self)
        {
            UseShellExecute = false,
            CreateNoWindow = true, // a hidden console of its own, which it then frees (count 1)
            WorkingDirectory = Path.GetDirectoryName(self)!,
        };
        using var _ = Process.Start(psi);
        return false;
    }

    // UTF-8 for the command line's output (instance names in Chinese, ...).
    public static void ForCommandLine()
    {
        if (PInvoke.GetConsoleWindow() == default)
            return;
        PInvoke.SetConsoleOutputCP(65001);
        PInvoke.SetConsoleCP(65001);
        try
        {
            Console.OutputEncoding = new System.Text.UTF8Encoding(false);
        }
        catch (IOException)
        {
        }
    }

    private static unsafe uint ConsoleProcessCount()
    {
        uint* list = stackalloc uint[8];
        return PInvoke.GetConsoleProcessList(new Span<uint>(list, 8));
    }
}
