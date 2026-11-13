using Windows.Win32;
using Windows.Win32.Storage.FileSystem;
using Windows.Win32.System.Console;

namespace BeDD;

// BeDD.exe is a GUI-subsystem program, so it starts without a console. For the command line it
// attaches to the caller's console (as LeviLauncher.exe does); streams the caller redirected to a
// file or pipe are kept. Started from Explorer with arguments there is no parent console: output is
// discarded and the exit code still reports the outcome.
internal static class ConsoleSetup
{
    private const uint AttachParentProcess = unchecked((uint)-1);

    public static void AttachToParent()
    {
        bool outOk = Valid(STD_HANDLE.STD_OUTPUT_HANDLE);
        bool errOk = Valid(STD_HANDLE.STD_ERROR_HANDLE);
        if (outOk && errOk)
            return;
        if (!PInvoke.AttachConsole(AttachParentProcess) && PInvoke.GetConsoleWindow() == default)
            return;
        PInvoke.SetConsoleOutputCP(65001);
        PInvoke.SetConsoleCP(65001);
        if (!outOk)
            Console.SetOut(Open());
        if (!errOk)
            Console.SetError(Open());
    }

    private static TextWriter Open()
    {
        var stream = new FileStream("CONOUT$", FileMode.Open, FileAccess.Write);
        return new StreamWriter(stream, new System.Text.UTF8Encoding(false)) { AutoFlush = true };
    }

    private static bool Valid(STD_HANDLE which)
    {
        var h = PInvoke.GetStdHandle(which);
        if (h.IsNull || h == (nint)(-1))
            return false;
        return PInvoke.GetFileType(h) != FILE_TYPE.FILE_TYPE_UNKNOWN;
    }
}
