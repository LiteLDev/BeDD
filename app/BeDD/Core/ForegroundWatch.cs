using System.Globalization;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.System.Threading;
using Windows.Win32.UI.Accessibility;
using Windows.Win32.UI.WindowsAndMessaging;

namespace BeDD.Core;

// A top-level window as the watch reports it.
public sealed record WatchedWindow(nint Hwnd, uint Pid, string Process, string Title);

// Watches the desktop for what a background game must never do: take the foreground or clip the
// cursor. Every change of the foreground window (EVENT_SYSTEM_FOREGROUND, on a thread of its own
// with a message loop) and of the cursor clip rectangle (GetClipCursor, polled every 100 ms) becomes
// one line; a line where a Minecraft window has the foreground is flagged. "BeDD.exe dev" writes
// these lines to tools\out\desktop-watch.log: no flagged line and the clip staying "full" is the
// evidence that the game left the user's mouse and keyboard alone.
public sealed class ForegroundWatch : IDisposable
{
    public const string MinecraftFlag = "  <-- MINECRAFT HAS THE FOREGROUND";
    private const uint PollMs = 100;

    private readonly Action<string> _sink;
    private readonly Func<DateTime> _clock;
    private readonly ManualResetEventSlim _started = new();
    private Thread? _thread;
    private uint _threadId;
    private string _lastForeground = "";
    private string _lastClip = "";
    private int _minecraftForegrounds;
    // Kept alive for as long as the hook is installed: the native side holds a pointer to it.
    private WINEVENTPROC? _proc;
    private Exception? _startError;

    // sink gets each line (without a line break) on the watch thread.
    public ForegroundWatch(Action<string> sink, Func<DateTime>? clock = null)
    {
        _sink = sink;
        _clock = clock ?? (() => DateTime.Now);
    }

    // How many times a Minecraft window became the foreground since Start.
    public int MinecraftForegrounds => Volatile.Read(ref _minecraftForegrounds);

    public void Start()
    {
        if (_thread is not null)
            throw new InvalidOperationException("the watch is already running");
        _thread = new Thread(Loop) { IsBackground = true, Name = "BeDD foreground watch" };
        _thread.Start();
        _started.Wait();
        if (_startError is not null)
            throw _startError;
    }

    public void Dispose()
    {
        if (_thread is { IsAlive: true })
        {
            PInvoke.PostThreadMessage(_threadId, PInvoke.WM_QUIT, default, default);
            _thread.Join(2000);
        }
        _started.Dispose();
    }

    private void Loop()
    {
        HWINEVENTHOOK hook = default;
        nuint timer = 0;
        try
        {
            _threadId = PInvoke.GetCurrentThreadId();
            // Creates this thread's message queue before Start returns, so that WM_QUIT is never lost.
            PInvoke.PeekMessage(out _, HWND.Null, 0, 0, PEEK_MESSAGE_REMOVE_TYPE.PM_NOREMOVE);
            _proc = OnWinEvent;
            hook = PInvoke.SetWinEventHook(PInvoke.EVENT_SYSTEM_FOREGROUND, PInvoke.EVENT_SYSTEM_FOREGROUND,
                HINSTANCE.Null, _proc, 0, 0, PInvoke.WINEVENT_OUTOFCONTEXT | PInvoke.WINEVENT_SKIPOWNPROCESS);
            if (hook.IsNull)
                throw new System.ComponentModel.Win32Exception("SetWinEventHook failed");
            timer = PInvoke.SetTimer(HWND.Null, 0, PollMs, null);
            var now = _clock();
            var fg = Describe(Window(PInvoke.GetForegroundWindow()));
            var clip = CurrentClip();
            _lastForeground = fg;
            _lastClip = clip;
            _sink(Header(now, fg, clip));
        }
        catch (Exception e)
        {
            _startError = e;
            if (!hook.IsNull)
                PInvoke.UnhookWinEvent(hook);
            _started.Set();
            return;
        }
        _started.Set();
        try
        {
            while (PInvoke.GetMessage(out var msg, HWND.Null, 0, 0) > 0)
            {
                if (msg.message == PInvoke.WM_TIMER && msg.hwnd.IsNull)
                {
                    PollClip();
                    continue;
                }
                PInvoke.TranslateMessage(msg);
                PInvoke.DispatchMessage(msg);
            }
        }
        finally
        {
            if (timer != 0)
                PInvoke.KillTimer(HWND.Null, timer);
            PInvoke.UnhookWinEvent(hook);
        }
    }

    private void OnWinEvent(HWINEVENTHOOK hook, uint ev, HWND hwnd, int idObject, int idChild, uint thread, uint time)
    {
        try
        {
            var w = Window(hwnd);
            var text = Describe(w);
            if (text == _lastForeground)
                return;
            _lastForeground = text;
            bool minecraft = w is not null && IsMinecraft(w);
            if (minecraft)
                Interlocked.Increment(ref _minecraftForegrounds);
            Emit(Line(_clock(), "foreground", text, minecraft));
            PollClip(); // a game that takes the foreground often clips the cursor right after
        }
        catch (Exception)
        {
            // An exception must not unwind into user32: in NativeAOT that ends the process.
        }
    }

    private void PollClip()
    {
        var clip = CurrentClip();
        if (clip == _lastClip)
            return;
        _lastClip = clip;
        Emit(Line(_clock(), "clip", clip));
    }

    // The sink runs on the watch thread, under a native callback: a failing sink (a closed console
    // or log) loses that line but never the process.
    private void Emit(string line)
    {
        try
        {
            _sink(line);
        }
        catch (Exception)
        {
        }
    }

    // ---- Formatting (pure, unit-tested) ----

    public static string Header(DateTime time, string foreground, string clip) =>
        $"# {time.ToString("o", CultureInfo.InvariantCulture)} watch started; foreground: {foreground}; clip: {clip}";

    public static string Line(DateTime time, string kind, string text, bool minecraft = false) =>
        $"{time.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture)} {kind}: {text}{(minecraft ? MinecraftFlag : "")}";

    public static string Describe(WatchedWindow? w) =>
        w is null ? "<none>" : $"{w.Process} (pid {w.Pid}) '{w.Title}'";

    // The game is Minecraft.Windows.exe (GDK); its window is titled "Minecraft".
    public static bool IsMinecraft(WatchedWindow w) =>
        w.Process.StartsWith("Minecraft", StringComparison.OrdinalIgnoreCase) ||
        w.Title.Equals("Minecraft", StringComparison.OrdinalIgnoreCase) ||
        w.Title.StartsWith("Minecraft ", StringComparison.OrdinalIgnoreCase);

    // "full" when the cursor may roam the whole virtual desktop, else the clip rectangle
    // "left,top-right,bottom".
    public static string ClipText(int left, int top, int right, int bottom, int vx, int vy, int vw, int vh) =>
        left <= vx && top <= vy && right >= vx + vw && bottom >= vy + vh
            ? "full"
            : string.Create(CultureInfo.InvariantCulture, $"{left},{top}-{right},{bottom}");

    // ---- Win32 ----

    // The foreground window's handle as a number (0 when none), e.g. for BEDD_RETURN_FOCUS.
    public static unsafe nint ForegroundHandle() => (nint)PInvoke.GetForegroundWindow().Value;

    private static string CurrentClip()
    {
        if (!PInvoke.GetClipCursor(out RECT r))
            return "?";
        return ClipText(r.left, r.top, r.right, r.bottom,
            PInvoke.GetSystemMetrics(SYSTEM_METRICS_INDEX.SM_XVIRTUALSCREEN),
            PInvoke.GetSystemMetrics(SYSTEM_METRICS_INDEX.SM_YVIRTUALSCREEN),
            PInvoke.GetSystemMetrics(SYSTEM_METRICS_INDEX.SM_CXVIRTUALSCREEN),
            PInvoke.GetSystemMetrics(SYSTEM_METRICS_INDEX.SM_CYVIRTUALSCREEN));
    }

    internal static unsafe WatchedWindow? Window(HWND hwnd)
    {
        if (hwnd.IsNull)
            return null;
        uint pid;
        PInvoke.GetWindowThreadProcessId(hwnd, &pid);
        string title;
        Span<char> buf = stackalloc char[256];
        fixed (char* p = buf)
        {
            int n = PInvoke.GetWindowText(hwnd, p, buf.Length);
            title = new string(p, 0, Math.Max(n, 0));
        }
        return new WatchedWindow(hwnd, pid, ProcessName(pid) ?? "?", title);
    }

    // The process's file name without ".exe", e.g. "Minecraft.Windows"; null when it cannot be read.
    internal static string? ProcessName(uint pid)
    {
        string? path = ProcessPath(pid);
        return path is null ? null : Path.GetFileNameWithoutExtension(path);
    }

    internal static string? ProcessPath(uint pid)
    {
        if (pid == 0)
            return null;
        using var h = PInvoke.OpenProcess_SafeHandle(PROCESS_ACCESS_RIGHTS.PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (h.IsInvalid)
            return null;
        Span<char> buf = stackalloc char[1024];
        uint size = (uint)buf.Length;
        if (!PInvoke.QueryFullProcessImageName(h, PROCESS_NAME_FORMAT.PROCESS_NAME_WIN32, buf, ref size))
            return null;
        return new string(buf[..(int)size]);
    }
}
