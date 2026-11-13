namespace BeDD;

internal static class Program
{
    // Without arguments (a double-click in Explorer) BeDD.exe is the manager window; with
    // arguments it is the command line (see ConsoleSetup for how one console program does both).
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 0)
            return ConsoleSetup.PrepareForGui() ? Gui.ManagerApp.Run() : 0;
        ConsoleSetup.ForCommandLine();
        return Cli.Dispatcher.Run(args, Console.Out, Console.Error);
    }
}
