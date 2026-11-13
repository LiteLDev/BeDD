using BeDD.Core;
using Windows.Win32;
using Windows.Win32.UI.HiDpi;
using Windows.Win32.UI.WindowsAndMessaging;

namespace BeDD.Gui;

// The manager window: one per logon session (per BeDD home, see ManagerInstance), it shows every
// BeDD game and closes them all when it is closed. It connects to every game it finds in the run
// folder and attaches as its manager: the game window stays off screen, off the taskbar and out of
// Alt+Tab, and the manager shows a live picture of it instead (a game whose manager connection
// drops closes, so does one that no manager attaches to within 30 s). The stop event closes every
// game without asking and exits (`BeDD.exe manager stop`). Each game is put into a kill-on-close job of the manager, so the games also go
// when the manager is killed. Logs to <BeDD home>\logs\manager.log.
internal static class ManagerApp
{
    public static int Run()
    {
        ManagerLog.Trim();
        using var single = new Mutex(false, ManagerInstance.MutexName);
        bool owned;
        try
        {
            owned = single.WaitOne(0);
        }
        catch (AbandonedMutexException)
        {
            owned = true;
        }
        if (!owned)
        {
            // Started again (a second double-click): show the one that runs, once its window is up.
            // One that is closing still holds the mutex for a moment (it hid its window): wait for
            // it and take over.
            var deadline = DateTime.UtcNow.AddSeconds(30);
            while (!owned && DateTime.UtcNow < deadline)
            {
                var (hwnd, _, visible) = ManagerInstance.FindWindow();
                if (hwnd != 0 && visible)
                {
                    var h = (Windows.Win32.Foundation.HWND)hwnd;
                    PInvoke.ShowWindow(h, SHOW_WINDOW_CMD.SW_RESTORE);
                    PInvoke.SetForegroundWindow(h);
                    return 0;
                }
                try
                {
                    owned = single.WaitOne(250);
                }
                catch (AbandonedMutexException)
                {
                    owned = true;
                }
            }
            if (!owned)
            {
                ManagerLog.Write("another manager runs in this session; exiting");
                return 1;
            }
        }
        try
        {
            if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 15063))
            {
                ManagerLog.Write("the manager needs Windows 10 1703 or later");
                return 1;
            }
            return RunOwned();
        }
        finally
        {
            single.ReleaseMutex();
        }
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows10.0.15063")]
    private static int RunOwned()
    {
        AppDomain.CurrentDomain.UnhandledException += (_, e) => ManagerLog.Write("fatal: " + e.ExceptionObject);
        PInvoke.SetProcessDpiAwarenessContext(new DPI_AWARENESS_CONTEXT(-4)); // per monitor v2
        PInvoke.SetCurrentProcessExplicitAppUserModelID("LiteLDev.BeDD.Manager");
        using var stop = new EventWaitHandle(false, EventResetMode.AutoReset, ManagerInstance.StopEventName);
        using var monitor = new GameMonitor();
        var window = new ManagerWindow(monitor);
        if (!window.Create())
            return 1;
        ManagerLog.Write($"manager started (home {Paths.Root}, watching {Paths.Run})");
        var stopWait = ThreadPool.RegisterWaitForSingleObject(stop, (_, _) =>
        {
            ManagerLog.Write("stop event signalled");
            window.PostStop();
        }, null, Timeout.Infinite, executeOnlyOnce: true);
        monitor.Start();
        ManagerWindow.RunLoop();
        stopWait.Unregister(null);
        ManagerLog.Write("manager exited");
        return 0;
    }
}
