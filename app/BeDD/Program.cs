namespace BeDD;

internal static class Program
{
    // Without arguments (a double-click in Explorer) BeDD.exe is the manager window; with
    // arguments it is the command line.
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 0)
            return Gui.ManagerApp.Run();
        ConsoleSetup.AttachToParent();
        return Cli.Dispatcher.Run(args, Console.Out, Console.Error);
    }
}
