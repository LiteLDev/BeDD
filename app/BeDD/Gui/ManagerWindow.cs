using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using BeDD.Core;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.Graphics.Dwm;
using Windows.Win32.Graphics.Gdi;
using Windows.Win32.System.Memory;
using Windows.Win32.UI.Input;
using Windows.Win32.UI.Input.Ime;
using Windows.Win32.UI.Input.KeyboardAndMouse;
using Windows.Win32.UI.WindowsAndMessaging;

namespace BeDD.Gui;

// The manager window, plain Win32: a header line, then a tile per game with a caption (dot, title,
// instance and pid, status) above a live DWM thumbnail of the game window. Double-click a tile's
// caption to show only that game (again: all), right-click it for its menu. Closing the window
// closes every game (it asks first, unless the stop event asked).
//
// Click a game's picture to control it, as in a virtual machine: the window's keyboard and mouse go
// to that game (InputForwarder) and the pointer stays inside its picture, hidden while the game
// holds the mouse (in a world, where relative movement turns the camera). The system's own keys
// (Windows key, Alt+Tab, Alt+Esc, Ctrl+Esc) go to the game too, through a low-level keyboard hook
// that lives only while a game is controlled and this window is in front. Ctrl+Left Alt (VMware's
// release keys; Minecraft binds no Ctrl+Alt chord, and AltGr is Ctrl+Right Alt, so typing is safe)
// releases the keyboard and mouse, as does anything that takes the focus from this window.
[System.Runtime.Versioning.SupportedOSPlatform("windows10.0.15063")]
internal sealed unsafe class ManagerWindow
{
    private const uint WM_DESTROY = 0x0002, WM_SIZE = 0x0005, WM_PAINT = 0x000F, WM_CLOSE = 0x0010,
        WM_QUERYENDSESSION = 0x0011, WM_ERASEBKGND = 0x0014, WM_ENDSESSION = 0x0016, WM_GETMINMAXINFO = 0x0024,
        WM_TIMER = 0x0113, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONUP = 0x0205, WM_DPICHANGED = 0x02E0, WM_APP = 0x8000;
    private const uint WM_MOVE = 0x0003, WM_ACTIVATE = 0x0006, WM_KILLFOCUS = 0x0008, WM_SETCURSOR = 0x0020, WM_INPUT = 0x00FF,
        WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_DEADCHAR = 0x0103, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105,
        WM_SYSCHAR = 0x0106, WM_SYSDEADCHAR = 0x0107, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
        WM_RBUTTONDOWN = 0x0204, WM_RBUTTONDBLCLK = 0x0206, WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MBUTTONDBLCLK = 0x0209,
        WM_MOUSEWHEEL = 0x020A, WM_XBUTTONDOWN = 0x020B, WM_XBUTTONUP = 0x020C, WM_XBUTTONDBLCLK = 0x020D;
    private const int VK_TAB = 0x09, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_ESCAPE = 0x1B, VK_LWIN = 0x5B, VK_RWIN = 0x5C,
        VK_F4 = 0x73, VK_LSHIFT = 0xA0, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_PACKET = 0xE7;
    private const int HTCLIENT = 1, WheelDelta = 120;
    private const uint IACE_DEFAULT = 0x10;
    private const uint LLKHF_EXTENDED = 0x01, LLKHF_UP = 0x80;
    private const uint WM_APP_CHANGED = WM_APP + 1, WM_APP_STOP = WM_APP + 2, WM_APP_EXIT = WM_APP + 3;
    private const uint DWM_TNP_RECTDESTINATION = 0x1, DWM_TNP_OPACITY = 0x4, DWM_TNP_VISIBLE = 0x8, DWM_TNP_SOURCECLIENTAREAONLY = 0x10;
    private const int IdcArrow = 32512, AppIconId = 32512, CfUnicodeText = 13;
    private const nint ENDSESSION_CLOSEAPP = 0x1;
    private const int CmdSolo = 1, CmdLogs = 2, CmdFolder = 3, CmdCopyUrl = 4, CmdClose = 5, CmdControl = 6, CmdSound = 7;

    private static readonly COLORREF Back = Rgb(24, 24, 27), CaptionBack = Rgb(39, 39, 42), BodyBack = Rgb(0, 0, 0);
    private static readonly COLORREF Text1 = Rgb(244, 244, 245), Text2 = Rgb(161, 161, 170);
    private static readonly COLORREF ActiveDot = Rgb(74, 222, 128), IdleDot = Rgb(113, 113, 122), BadDot = Rgb(248, 113, 113);
    private static readonly COLORREF ControlledCaption = Rgb(30, 64, 175), Accent = Rgb(147, 197, 253);

    private static ManagerWindow? s_current;
    // Kept alive for as long as the class is registered.
    private static readonly WNDPROC s_wndProc = WndProc;
    private static readonly HOOKPROC s_keyboardProc = KeyboardProc;

    private readonly GameMonitor _monitor;
    private readonly HashSet<Game> _thumbed = [];
    private HWND _hwnd;
    private uint _dpi = 96;
    private HFONT _font, _bold, _small;
    private int _focusPid;
    private int _changePosted;
    private bool _closing;

    // The game the user controls, and what goes with it (UI thread only).
    private Game? _input;
    private InputForwarder? _forwarder;
    private UnhookWindowsHookExSafeHandle? _keyboardHook;
    private int _wheel;
    private bool _eatLeftUp;
    private readonly HashSet<int> _hookKeys = [];

    public ManagerWindow(GameMonitor monitor)
    {
        _monitor = monitor;
        _monitor.Changed += () =>
        {
            if (Interlocked.Exchange(ref _changePosted, 1) == 0)
                PInvoke.PostMessage(_hwnd, WM_APP_CHANGED, 0, 0);
        };
    }

    public static string SettingsFile => Path.Combine(Paths.Root, "manager.json");

    private static COLORREF Rgb(byte r, byte g, byte b) => new((uint)(r | (g << 8) | (b << 16)));

    // Creates and shows the window (without taking the focus). False when Windows refuses.
    public bool Create()
    {
        s_current = this;
        var module = PInvoke.GetModuleHandle((PCWSTR)null);
        string className = ManagerInstance.WindowClass;
        fixed (char* cls = className)
        {
            var wc = new WNDCLASSEXW
            {
                cbSize = (uint)Marshal.SizeOf<WNDCLASSEXW>(),
                style = WNDCLASS_STYLES.CS_DBLCLKS | WNDCLASS_STYLES.CS_HREDRAW | WNDCLASS_STYLES.CS_VREDRAW,
                lpfnWndProc = s_wndProc,
                hInstance = module,
                hCursor = PInvoke.LoadCursor(default, (PCWSTR)(char*)IdcArrow),
                hIcon = LoadIcon(module, 32),
                hIconSm = LoadIcon(module, 16),
                lpszClassName = cls,
            };
            if (PInvoke.RegisterClassEx(in wc) == 0)
            {
                ManagerLog.Write("RegisterClassEx failed (" + Marshal.GetLastWin32Error() + ")");
                return false;
            }
        }

        var (x, y, w, h) = DefaultBounds();
        fixed (char* cls = className)
        fixed (char* title = "BeDD")
            _hwnd = PInvoke.CreateWindowEx(0, cls, title, WINDOW_STYLE.WS_OVERLAPPEDWINDOW | WINDOW_STYLE.WS_CLIPCHILDREN,
                x, y, w, h, default, default, module, null);
        if (_hwnd.IsNull)
        {
            ManagerLog.Write("CreateWindowEx failed (" + Marshal.GetLastWin32Error() + ")");
            return false;
        }
        _dpi = Math.Max(96u, PInvoke.GetDpiForWindow(_hwnd));
        MakeFonts();
        int dark = 1;
        PInvoke.DwmSetWindowAttribute(_hwnd, DWMWINDOWATTRIBUTE.DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(int));
        RestorePlacement();
        // Opened by a launcher on someone's desktop: never take the focus from what they are doing.
        PInvoke.ShowWindow(_hwnd, SHOW_WINDOW_CMD.SW_SHOWNOACTIVATE);
        PInvoke.SetTimer(_hwnd, 1, 1000, null);
        return true;
    }

    public nint Handle => _hwnd;

    // The message loop; returns when the window is gone.
    public static void RunLoop()
    {
        while (PInvoke.GetMessage(out var msg, default, 0, 0) > 0)
        {
            PInvoke.TranslateMessage(in msg);
            PInvoke.DispatchMessage(in msg);
        }
    }

    // Asks the window to close every game without asking and exit (from any thread).
    public void PostStop() => PInvoke.PostMessage(_hwnd, WM_APP_STOP, 0, 0);

    private static HICON LoadIcon(HMODULE module, int size)
    {
        var h = PInvoke.LoadImage(module, (PCWSTR)(char*)AppIconId, GDI_IMAGE_TYPE.IMAGE_ICON, size, size, IMAGE_FLAGS.LR_DEFAULTCOLOR);
        return new HICON(h.Value);
    }

    private static (int X, int Y, int W, int H) DefaultBounds()
    {
        RECT work;
        if (!PInvoke.SystemParametersInfo(SYSTEM_PARAMETERS_INFO_ACTION.SPI_GETWORKAREA, 0, &work, 0))
            work = new RECT(0, 0, 1280, 720);
        int ww = work.right - work.left, wh = work.bottom - work.top;
        return (work.left + ww / 5, work.top + wh / 6, ww * 3 / 5, wh * 2 / 3);
    }

    private int S(int v) => (int)Math.Round(v * _dpi / 96.0);

    private void MakeFonts()
    {
        DeleteFonts();
        _font = Font(9f, 400);
        _bold = Font(9.5f, 700);
        _small = Font(8.5f, 400);
    }

    private HFONT Font(float points, int weight)
    {
        fixed (char* face = "Segoe UI")
            return PInvoke.CreateFont(-(int)Math.Round(points * _dpi / 72.0), 0, 0, 0, weight, 0, 0, 0, FONT_CHARSET.DEFAULT_CHARSET,
                FONT_OUTPUT_PRECISION.OUT_DEFAULT_PRECIS, FONT_CLIP_PRECISION.CLIP_DEFAULT_PRECIS, FONT_QUALITY.CLEARTYPE_QUALITY, 0, face);
    }

    private void DeleteFonts()
    {
        foreach (var f in new[] { _font, _bold, _small })
            if (!f.IsNull)
                PInvoke.DeleteObject(f);
        _font = _bold = _small = default;
    }

    private static LRESULT WndProc(HWND hwnd, uint msg, WPARAM wParam, LPARAM lParam)
    {
        var self = s_current;
        if (self is not null && (self._hwnd == hwnd || self._hwnd.IsNull))
        {
            try
            {
                if (self.OnMessage(hwnd, msg, wParam, lParam, out var result))
                    return result;
            }
            catch (Exception e)
            {
                ManagerLog.Write("UI error: " + e);
            }
        }
        return PInvoke.DefWindowProc(hwnd, msg, wParam, lParam);
    }

    private bool OnMessage(HWND hwnd, uint msg, WPARAM wParam, LPARAM lParam, out LRESULT result)
    {
        result = default;
        if (_forwarder is not null && OnControlMessage(msg, wParam, lParam, out result))
            return true;
        switch (msg)
        {
            case WM_ERASEBKGND:
                result = (LRESULT)1;
                return true;
            case WM_PAINT:
                Paint(hwnd);
                return true;
            case WM_SIZE:
                Relayout();
                return true;
            case WM_TIMER:
                PInvoke.InvalidateRect(hwnd, (RECT*)null, false);
                return true;
            case WM_GETMINMAXINFO:
                var mmi = (MINMAXINFO*)(nint)lParam;
                mmi->ptMinTrackSize = new Point(S(360), S(240));
                return true;
            case WM_DPICHANGED:
                _dpi = Math.Max(96u, (uint)((nuint)wParam & 0xFFFF));
                MakeFonts();
                var r = (RECT*)(nint)lParam;
                PInvoke.SetWindowPos(hwnd, default, r->left, r->top, r->right - r->left, r->bottom - r->top,
                    SET_WINDOW_POS_FLAGS.SWP_NOZORDER | SET_WINDOW_POS_FLAGS.SWP_NOACTIVATE);
                Relayout();
                return true;
            case WM_APP_CHANGED:
                Volatile.Write(ref _changePosted, 0);
                Relayout();
                return true;
            case WM_LBUTTONDOWN:
                if (GameAt(lParam) is { } picked && picked.Dest.Contains(PointOf(lParam)))
                    Control(picked, PointOf(lParam));
                return true;
            case WM_ACTIVATE:
            case WM_KILLFOCUS:
                return false;
            case WM_LBUTTONDBLCLK:
                if (GameAt(lParam) is { } solo && solo.Caption.Contains(PointOf(lParam)))
                {
                    _focusPid = _focusPid == solo.Pid ? 0 : solo.Pid;
                    Relayout();
                }
                return true;
            case WM_RBUTTONUP:
                if (GameAt(lParam) is { } g)
                    ShowMenu(g, lParam);
                return true;
            case WM_CLOSE:
                // The stop event may have come while the question was open.
                if (!_closing && Confirm() && !_closing)
                    BeginShutdown("closed by the user");
                return true;
            case WM_APP_STOP:
                if (!_closing)
                    BeginShutdown("stop requested");
                return true;
            case WM_APP_EXIT:
                PInvoke.DestroyWindow(hwnd);
                return true;
            case WM_QUERYENDSESSION:
                result = (LRESULT)1;
                return true;
            case WM_ENDSESSION:
                if (wParam != 0 && !_closing)
                {
                    _closing = true;
                    // ENDSESSION_CLOSEAPP: the Restart Manager (an installer replacing BeDD.exe) closes
                    // this app alone and waits for it to exit; at a real shutdown the process just ends.
                    bool closeApp = ((nint)lParam & ENDSESSION_CLOSEAPP) != 0;
                    ManagerLog.Write(closeApp ? "closed by the restart manager (an installer replaces BeDD.exe)" : "windows is shutting down");
                    SavePlacement();
                    _monitor.CloseAll();
                    if (closeApp)
                        PInvoke.DestroyWindow(hwnd);
                }
                return true;
            case WM_DESTROY:
                Release("the window closed");
                PInvoke.KillTimer(hwnd, 1);
                DropAllThumbs();
                DeleteFonts();
                PInvoke.PostQuitMessage(0);
                return true;
        }
        return false;
    }

    // The close button, Alt+F4, the taskbar: ask when there are games.
    private bool Confirm()
    {
        int n;
        lock (_monitor.Sync)
            n = _monitor.Games.Count;
        if (n == 0)
            return true;
        string what = n == 1 ? "the Minecraft game it shows" : "the " + n.ToString(CultureInfo.InvariantCulture) + " Minecraft games it shows";
        var answer = PInvoke.MessageBox(_hwnd, "Closing this window also closes " + what + ".\n\nTest runs still driving them will fail.", "BeDD",
            MESSAGEBOX_STYLE.MB_OKCANCEL | MESSAGEBOX_STYLE.MB_ICONWARNING | MESSAGEBOX_STYLE.MB_DEFBUTTON2);
        return answer == MESSAGEBOX_RESULT.IDOK;
    }

    // Hides the window, closes every game on a worker thread, then destroys the window.
    private void BeginShutdown(string why)
    {
        Release("shutting down");
        _closing = true;
        ManagerLog.Write("shutting down: " + why);
        SavePlacement();
        DropAllThumbs();
        PInvoke.ShowWindow(_hwnd, SHOW_WINDOW_CMD.SW_HIDE);
        var hwnd = _hwnd;
        Task.Run(() =>
        {
            try
            {
                _monitor.CloseAll();
            }
            catch (Exception e)
            {
                ManagerLog.Write("closing the games: " + e);
            }
            PInvoke.PostMessage(hwnd, WM_APP_EXIT, 0, 0);
        });
    }

    // --- controlling a game ------------------------------------------------------------------------

    // The window's messages while the user controls a game: input goes to the game, not to this
    // window (so Alt+F4, Alt+Space and F10 do nothing here either). False: handled as usual.
    private bool OnControlMessage(uint msg, WPARAM wParam, LPARAM lParam, out LRESULT result)
    {
        result = default;
        var fwd = _forwarder!;
        var g = _input!;
        switch (msg)
        {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP:
                OnKey(fwd, msg is WM_KEYDOWN or WM_SYSKEYDOWN, wParam, lParam);
                return true;
            case WM_CHAR:
                int c = (int)(nuint)wParam;
                if (c is > 0 and <= 0xFFFF)
                    fwd.Char(c);
                return true;
            case WM_SYSCHAR:
            case WM_DEADCHAR:
            case WM_SYSDEADCHAR:
                return true;
            case WM_MOUSEMOVE:
                bool grabbed;
                lock (_monitor.Sync)
                    grabbed = g.Grabbed;
                // While the game holds the mouse it turns the camera with relative movement (WM_INPUT).
                if (!grabbed && g.ToGame(PointOf(lParam)) is { } p)
                    fwd.Move(p.X, p.Y);
                return true;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                fwd.Button("left", true);
                return true;
            case WM_LBUTTONUP:
                if (!_eatLeftUp)
                    fwd.Button("left", false);
                _eatLeftUp = false;
                return true;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
                fwd.Button("right", true);
                return true;
            case WM_RBUTTONUP:
                fwd.Button("right", false);
                return true;
            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
                fwd.Button("middle", true);
                return true;
            case WM_MBUTTONUP:
                fwd.Button("middle", false);
                return true;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONDBLCLK:
            case WM_XBUTTONUP:
                fwd.Button((((nuint)wParam >> 16) & 0xFFFF) == 1 ? "x1" : "x2", msg != WM_XBUTTONUP);
                result = (LRESULT)1;
                return true;
            case WM_MOUSEWHEEL:
                _wheel += (short)(((nuint)wParam >> 16) & 0xFFFF);
                int notches = _wheel / WheelDelta;
                _wheel -= notches * WheelDelta;
                fwd.Wheel(notches);
                return true;
            case WM_INPUT:
                OnRawInput(fwd, lParam);
                return false; // DefWindowProc cleans up
            case WM_SETCURSOR:
                bool hide;
                lock (_monitor.Sync)
                    hide = g.Grabbed;
                if (!hide || ((nuint)(nint)lParam & 0xFFFF) != HTCLIENT)
                    return false;
                PInvoke.SetCursor(default);
                result = (LRESULT)1;
                return true;
            case WM_ACTIVATE:
                if (((nuint)wParam & 0xFFFF) == 0) // WA_INACTIVE
                    Release("another window took the focus");
                return false;
            case WM_KILLFOCUS:
                Release("the window lost the keyboard");
                return false;
            case WM_MOVE:
                Clip();
                return false;
        }
        return false;
    }

    private void OnKey(InputForwarder fwd, bool down, WPARAM wParam, LPARAM lParam)
    {
        int vk = (int)(nuint)wParam;
        long lp = lParam;
        // Ctrl + Left Alt: give the keyboard and mouse back (AltGr is Ctrl + Right Alt).
        if (down && PInvoke.GetKeyState(VK_CONTROL) < 0 && PInvoke.GetKeyState(VK_LMENU) < 0)
        {
            Release("Ctrl+Alt");
            return;
        }
        bool extended = ((lp >> 24) & 1) != 0;
        vk = vk switch
        {
            VK_SHIFT => ShiftOf((uint)((lp >> 16) & 0xFF)),
            VK_CONTROL => extended ? VK_RCONTROL : VK_LCONTROL,
            VK_MENU => extended ? VK_RMENU : VK_LMENU,
            _ => vk,
        };
        if (vk is <= 0 or >= 0xFF or VK_PACKET)
            return;
        // Alt+F4 closes neither this window nor the game (whose window procedure would close it).
        if (vk == VK_F4 && down && PInvoke.GetKeyState(VK_MENU) < 0)
            return;
        fwd.Key(vk, down);
    }

    private static int ShiftOf(uint scanCode)
    {
        int vk = (int)PInvoke.MapVirtualKey(scanCode, MAP_VIRTUAL_KEY_TYPE.MAPVK_VSC_TO_VK_EX);
        return vk is 0xA0 or 0xA1 ? vk : VK_LSHIFT;
    }

    private void OnRawInput(InputForwarder fwd, LPARAM lParam)
    {
        RAWINPUT raw;
        uint size = (uint)sizeof(RAWINPUT);
        uint got = PInvoke.GetRawInputData(new HRAWINPUT((nint)lParam), RAW_INPUT_DATA_COMMAND_FLAGS.RID_INPUT, (void*)&raw, &size,
            (uint)sizeof(RAWINPUTHEADER));
        if (got == uint.MaxValue || got < (uint)sizeof(RAWINPUTHEADER) || raw.header.dwType != 0) // RIM_TYPEMOUSE
            return;
        if (((uint)raw.data.mouse.usFlags & 0x1) != 0) // MOUSE_MOVE_ABSOLUTE (a remote desktop, a tablet)
            return;
        fwd.Raw(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
    }

    // Gives the window's keyboard and mouse to the game (a click on its picture, or its menu).
    private void Control(Game g, Point? at)
    {
        Release("another game");
        var fwd = _monitor.Forwarder(g);
        if (fwd is null)
            return;
        lock (_monitor.Sync)
        {
            if (g.Dest.IsEmpty)
                return;
            g.Controlled = true;
        }
        _input = g;
        _forwarder = fwd;
        _wheel = 0;
        _eatLeftUp = at is not null; // the click that took control is not the game's
        var mouse = new RAWINPUTDEVICE { usUsagePage = 0x01, usUsage = 0x02, dwFlags = 0, hwndTarget = _hwnd };
        if (!PInvoke.RegisterRawInputDevices(&mouse, 1, (uint)sizeof(RAWINPUTDEVICE)))
            ManagerLog.Write("RegisterRawInputDevices failed (" + Marshal.GetLastWin32Error() + "): no camera movement");
        _keyboardHook = PInvoke.SetWindowsHookEx(WINDOWS_HOOK_ID.WH_KEYBOARD_LL, s_keyboardProc, PInvoke.GetModuleHandle((string?)null), 0);
        if (_keyboardHook.IsInvalid)
            ManagerLog.Write("SetWindowsHookEx failed (" + Marshal.GetLastWin32Error() + "): the Windows key and Alt+Tab stay the system's");
        // No IME while the keys are the game's: W, A, S and D must not start a composition.
        PInvoke.ImmAssociateContextEx(_hwnd, default(HIMC), 0);
        Clip();
        if (at is { } p && g.ToGame(p) is { } gp)
            fwd.Move(gp.X, gp.Y);
        ManagerLog.Write($"the user controls game {g.Pid}");
        PInvoke.InvalidateRect(_hwnd, (RECT*)null, false);
    }

    // Gives the keyboard and mouse back to the window (and lets go of what the game holds).
    private void Release(string why)
    {
        if (_forwarder is not { } fwd || _input is not { } g)
            return;
        _forwarder = null;
        _input = null;
        lock (_monitor.Sync)
            g.Controlled = false;
        fwd.Release();
        PInvoke.ClipCursor((RECT*)null);
        var mouse = new RAWINPUTDEVICE { usUsagePage = 0x01, usUsage = 0x02, dwFlags = RAWINPUTDEVICE_FLAGS.RIDEV_REMOVE, hwndTarget = default };
        PInvoke.RegisterRawInputDevices(&mouse, 1, (uint)sizeof(RAWINPUTDEVICE));
        _keyboardHook?.Dispose();
        _keyboardHook = null;
        _hookKeys.Clear();
        PInvoke.ImmAssociateContextEx(_hwnd, default(HIMC), IACE_DEFAULT);
        ManagerLog.Write($"the user released game {g.Pid}: {why}");
        if (!_hwnd.IsNull)
            PInvoke.InvalidateRect(_hwnd, (RECT*)null, false);
    }

    // Keeps the pointer inside the controlled game's picture.
    private void Clip()
    {
        if (_input is not { } g)
            return;
        Rectangle dest;
        lock (_monitor.Sync)
            dest = g.Dest;
        if (dest.IsEmpty)
            return;
        var tl = new Point(dest.Left, dest.Top);
        var br = new Point(dest.Right, dest.Bottom);
        PInvoke.ClientToScreen(_hwnd, &tl);
        PInvoke.ClientToScreen(_hwnd, &br);
        var r = new RECT(tl.X, tl.Y, br.X, br.Y);
        PInvoke.ClipCursor(&r);
    }

    // The low-level keyboard hook (installed only while a game is controlled): the keys Windows
    // keeps for itself go to the game while this window is in front.
    private static LRESULT KeyboardProc(int code, WPARAM wParam, LPARAM lParam)
    {
        var self = s_current;
        if (code >= 0 && self is not null)
        {
            try
            {
                if (self.OnLowLevelKey((KBDLLHOOKSTRUCT*)(nint)lParam))
                    return (LRESULT)1;
            }
            catch (Exception e)
            {
                ManagerLog.Write("keyboard hook: " + e);
            }
        }
        return PInvoke.CallNextHookEx(null, code, wParam, lParam);
    }

    private bool OnLowLevelKey(KBDLLHOOKSTRUCT* k)
    {
        if (_forwarder is not { } fwd || PInvoke.GetForegroundWindow() != _hwnd)
            return false;
        int vk = (int)k->vkCode;
        bool up = ((uint)k->flags & LLKHF_UP) != 0;
        if (up)
        {
            if (!_hookKeys.Remove(vk))
                return false;
            fwd.Key(vk, false);
            return true;
        }
        bool alt = (PInvoke.GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool ctrl = (PInvoke.GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool system = vk is VK_LWIN or VK_RWIN || (vk == VK_TAB && alt) || (vk == VK_ESCAPE && (alt || ctrl));
        if (!system)
            return false;
        _hookKeys.Add(vk);
        fwd.Key(vk, true);
        return true;
    }

    // --- layout and thumbnails ---------------------------------------------------------------------

    private Rectangle ClientRect()
    {
        PInvoke.GetClientRect(_hwnd, out var rc);
        return Rectangle.FromLTRB(rc.left, rc.top, rc.right, rc.bottom);
    }

    private int HeaderHeight => S(34);

    private List<Game> Visible()
    {
        var all = _monitor.Games;
        if (_focusPid != 0 && all.Any(g => g.Pid == _focusPid))
            return all.Where(g => g.Pid == _focusPid).ToList();
        return [.. all];
    }

    private void Relayout()
    {
        if (_closing || _hwnd.IsNull)
            return;
        var client = ClientRect();
        lock (_monitor.Sync)
        {
            var shown = Visible();
            var area = Rectangle.FromLTRB(0, HeaderHeight, client.Width, client.Height);
            var tiles = TileLayout.Arrange(area, shown.Count, S(8), S(28));
            for (int i = 0; i < shown.Count; i++)
            {
                shown[i].Tile = tiles[i].Bounds;
                shown[i].Caption = tiles[i].Caption;
                shown[i].Body = tiles[i].Body;
            }
            foreach (var g in _monitor.Games)
            {
                bool visible = shown.Contains(g);
                if (!visible)
                    g.Tile = g.Caption = g.Body = Rectangle.Empty;
                UpdateThumb(g, visible);
            }
            // Games that went away.
            foreach (var g in _thumbed.Where(g => !_monitor.Games.Contains(g)).ToList())
                DropThumb(g);
            if (_focusPid != 0 && !_monitor.Games.Any(g => g.Pid == _focusPid))
                _focusPid = 0;
        }
        if (_input is { } controlled)
        {
            bool gone;
            lock (_monitor.Sync)
                gone = !_monitor.Games.Contains(controlled) || controlled.Dest.IsEmpty || controlled.Closing || !controlled.Attached;
            if (gone)
                Release("the game is no longer shown");
            else
                Clip();
        }
        PInvoke.InvalidateRect(_hwnd, (RECT*)null, false);
    }

    private void UpdateThumb(Game g, bool visible)
    {
        var source = (HWND)g.Hwnd;
        if (g.Hwnd == 0 || !PInvoke.IsWindow(source))
        {
            DropThumb(g);
            return;
        }
        if (g.Thumb == 0 || g.ThumbSource != g.Hwnd)
        {
            DropThumb(g);
            nint thumb;
            if (PInvoke.DwmRegisterThumbnail(_hwnd, source, &thumb).Failed)
                return;
            g.Thumb = thumb;
            g.ThumbSource = g.Hwnd;
            _thumbed.Add(g);
        }
        if (g.ClientSize.Width > 0 && g.ClientSize.Height > 0)
            g.Source = g.ClientSize;
        else if (PInvoke.DwmQueryThumbnailSourceSize(g.Thumb, out var size).Succeeded && size.cx > 0 && size.cy > 0)
            g.Source = new Size(size.cx, size.cy);
        var dest = TileLayout.Fit(g.Body, g.Source);
        g.Dest = visible ? dest : Rectangle.Empty;
        var p = new DWM_THUMBNAIL_PROPERTIES
        {
            dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY,
            rcDestination = new RECT(dest.Left, dest.Top, dest.Right, dest.Bottom),
            opacity = 255,
            fVisible = visible && dest.Width > 0 && dest.Height > 0,
            fSourceClientAreaOnly = true,
        };
        PInvoke.DwmUpdateThumbnailProperties(g.Thumb, in p);
    }

    private void DropThumb(Game g)
    {
        if (g.Thumb != 0)
            PInvoke.DwmUnregisterThumbnail(g.Thumb);
        g.Thumb = 0;
        g.ThumbSource = 0;
        g.Dest = Rectangle.Empty;
        _thumbed.Remove(g);
    }

    private void DropAllThumbs()
    {
        lock (_monitor.Sync)
            foreach (var g in _thumbed.ToList())
                DropThumb(g);
    }

    // --- painting ----------------------------------------------------------------------------------

    private void Paint(HWND hwnd)
    {
        var hdc = PInvoke.BeginPaint(hwnd, out var ps);
        try
        {
            var client = ClientRect();
            if (client.Width <= 0 || client.Height <= 0)
                return;
            // Into a bitmap first: no flicker.
            var mem = PInvoke.CreateCompatibleDC(hdc);
            var bmp = PInvoke.CreateCompatibleBitmap(hdc, client.Width, client.Height);
            var oldBmp = PInvoke.SelectObject(mem, bmp);
            try
            {
                Draw(mem, client);
                PInvoke.BitBlt(hdc, 0, 0, client.Width, client.Height, mem, 0, 0, ROP_CODE.SRCCOPY);
            }
            finally
            {
                PInvoke.SelectObject(mem, oldBmp);
                PInvoke.DeleteObject(bmp);
                PInvoke.DeleteDC(mem);
            }
        }
        finally
        {
            PInvoke.EndPaint(hwnd, in ps);
        }
    }

    private void Draw(HDC dc, Rectangle client)
    {
        Fill(dc, client, Back);
        PInvoke.SetBkMode(dc, BACKGROUND_MODE.TRANSPARENT);
        var now = DateTime.Now;
        lock (_monitor.Sync)
        {
            int n = _monitor.Games.Count;
            var header = new Rectangle(0, 0, client.Width, HeaderHeight);
            string count = n == 0 ? "No headless games" : n == 1 ? "1 headless game" : n.ToString(CultureInfo.InvariantCulture) + " headless games";
            if (_focusPid != 0 && n > 1)
                count += " (showing one: double-click its title to show all)";
            if (_input is { } controlled)
            {
                Text(dc, "Controlling " + controlled.Title, _bold, Text1, new Rectangle(S(12), 0, header.Width / 2 - S(12), header.Height),
                    DRAW_TEXT_FORMAT.DT_LEFT);
                Text(dc, "Press Ctrl+Left Alt to release the keyboard and mouse", _small, Accent,
                    new Rectangle(header.Width / 2, 0, header.Width / 2 - S(12), header.Height), DRAW_TEXT_FORMAT.DT_RIGHT);
            }
            else
            {
                Text(dc, count, _bold, Text1, new Rectangle(S(12), 0, header.Width / 2 - S(12), header.Height), DRAW_TEXT_FORMAT.DT_LEFT);
                Text(dc, n == 0 ? "Closing this window closes every game in it" : "Click a game to control it  ·  closing this window closes every game",
                    _small, Text2, new Rectangle(header.Width / 2, 0, header.Width / 2 - S(12), header.Height), DRAW_TEXT_FORMAT.DT_RIGHT);
            }
            if (n == 0)
            {
                var body = Rectangle.FromLTRB(0, header.Bottom, client.Width, client.Height);
                int line = S(20);
                int mid = body.Top + body.Height / 2;
                Text(dc, "Games started with BeDD show up here.", _font, Text2,
                    new Rectangle(0, mid - line, client.Width, line), DRAW_TEXT_FORMAT.DT_CENTER);
                Text(dc, "Click a game to control it (Ctrl+Left Alt releases it), double-click its title to enlarge it, right-click for more.", _font, Text2,
                    new Rectangle(0, mid, client.Width, line), DRAW_TEXT_FORMAT.DT_CENTER);
                return;
            }
            foreach (var g in Visible())
            {
                if (g.Tile.IsEmpty)
                    continue;
                Fill(dc, g.Caption, g.Controlled ? ControlledCaption : CaptionBack);
                Fill(dc, g.Body, BodyBack);
                var (status, kind) = g.Status(now);
                int d = S(8);
                Dot(dc, new Rectangle(g.Caption.Left + S(10), g.Caption.Top + (g.Caption.Height - d) / 2, d, d),
                    kind switch { GameStatusKind.Active => ActiveDot, GameStatusKind.Bad => BadDot, _ => IdleDot });
                var text = Rectangle.FromLTRB(g.Caption.Left + S(26), g.Caption.Top, g.Caption.Right - S(10), g.Caption.Bottom);
                int statusW = Math.Min(Measure(dc, status, _small), text.Width / 2);
                var statusBox = new Rectangle(text.Right - statusW, text.Top, statusW, text.Height);
                Text(dc, status, _small, Text2, statusBox, DRAW_TEXT_FORMAT.DT_RIGHT);
                int titleW = Math.Min(Measure(dc, g.Title, _bold), Math.Max(0, statusBox.Left - text.Left - S(12)));
                var titleBox = new Rectangle(text.Left, text.Top, titleW, text.Height);
                Text(dc, g.Title, _bold, Text1, titleBox, DRAW_TEXT_FORMAT.DT_LEFT);
                var subBox = Rectangle.FromLTRB(titleBox.Right + S(10), text.Top, statusBox.Left - S(12), text.Bottom);
                if (subBox.Width > S(30))
                    Text(dc, g.Subtitle, _small, Text2, subBox, DRAW_TEXT_FORMAT.DT_LEFT);
                if (g.Thumb == 0)
                {
                    string what = g.Closing ? "closing…" : !g.Connected ? "connecting…" : g.Hwnd == 0 ? "starting…" : "";
                    Text(dc, what, _font, Text2, g.Body, DRAW_TEXT_FORMAT.DT_CENTER);
                }
            }
        }
    }

    private static void Fill(HDC dc, Rectangle r, COLORREF color)
    {
        var brush = PInvoke.CreateSolidBrush(color);
        var rc = new RECT(r.Left, r.Top, r.Right, r.Bottom);
        PInvoke.FillRect(dc, &rc, brush);
        PInvoke.DeleteObject(brush);
    }

    private static void Dot(HDC dc, Rectangle r, COLORREF color)
    {
        var brush = PInvoke.CreateSolidBrush(color);
        var oldBrush = PInvoke.SelectObject(dc, brush);
        var oldPen = PInvoke.SelectObject(dc, PInvoke.GetStockObject(GET_STOCK_OBJECT_FLAGS.NULL_PEN));
        PInvoke.Ellipse(dc, r.Left, r.Top, r.Right + 1, r.Bottom + 1);
        PInvoke.SelectObject(dc, oldPen);
        PInvoke.SelectObject(dc, oldBrush);
        PInvoke.DeleteObject(brush);
    }

    private static void Text(HDC dc, string s, HFONT font, COLORREF color, Rectangle box, DRAW_TEXT_FORMAT align)
    {
        if (s.Length == 0 || box.Width <= 0 || box.Height <= 0)
            return;
        var old = PInvoke.SelectObject(dc, font);
        PInvoke.SetTextColor(dc, color);
        var rc = new RECT(box.Left, box.Top, box.Right, box.Bottom);
        fixed (char* p = s)
            PInvoke.DrawText(dc, p, s.Length, &rc,
                align | DRAW_TEXT_FORMAT.DT_SINGLELINE | DRAW_TEXT_FORMAT.DT_VCENTER | DRAW_TEXT_FORMAT.DT_END_ELLIPSIS | DRAW_TEXT_FORMAT.DT_NOPREFIX);
        PInvoke.SelectObject(dc, old);
    }

    private static int Measure(HDC dc, string s, HFONT font)
    {
        if (s.Length == 0)
            return 0;
        var old = PInvoke.SelectObject(dc, font);
        var rc = new RECT(0, 0, 0, 0);
        fixed (char* p = s)
            PInvoke.DrawText(dc, p, s.Length, &rc, DRAW_TEXT_FORMAT.DT_CALCRECT | DRAW_TEXT_FORMAT.DT_SINGLELINE | DRAW_TEXT_FORMAT.DT_NOPREFIX);
        PInvoke.SelectObject(dc, old);
        return rc.right - rc.left;
    }

    // --- mouse -------------------------------------------------------------------------------------

    private static Point PointOf(LPARAM lParam)
    {
        long v = lParam;
        return new Point((short)(v & 0xFFFF), (short)((v >> 16) & 0xFFFF));
    }

    private Game? GameAt(LPARAM lParam)
    {
        var p = PointOf(lParam);
        lock (_monitor.Sync)
            return Visible().FirstOrDefault(g => g.Tile.Contains(p));
    }

    private void ShowMenu(Game g, LPARAM lParam)
    {
        var menu = PInvoke.CreatePopupMenu();
        try
        {
            bool closing, attached, audio, shown;
            string instanceDir = g.Endpoint.InstanceDir;
            lock (_monitor.Sync)
            {
                closing = g.Closing;
                attached = g.Attached && g.Connected;
                audio = g.Audio;
                shown = !g.Dest.IsEmpty;
            }
            bool controlled = _input == g;
            Append(menu, CmdControl, controlled ? "Release the keyboard and mouse	Ctrl+Left Alt" : "Control this game	click it",
                controlled || (attached && shown && !closing));
            Append(menu, CmdSound, "Play its sound on this PC", attached && !closing, audio);
            PInvoke.AppendMenu(menu, MENU_ITEM_FLAGS.MF_SEPARATOR, 0, (PCWSTR)null);
            Append(menu, CmdSolo, _focusPid == g.Pid ? "Show all games" : "Show only this game", true);
            PInvoke.AppendMenu(menu, MENU_ITEM_FLAGS.MF_SEPARATOR, 0, (PCWSTR)null);
            Append(menu, CmdLogs, "Open log folder", instanceDir.Length > 0);
            Append(menu, CmdFolder, "Open instance folder", instanceDir.Length > 0);
            Append(menu, CmdCopyUrl, "Copy WebSocket URL (with token)", g.Endpoint.Url.Length > 0);
            PInvoke.AppendMenu(menu, MENU_ITEM_FLAGS.MF_SEPARATOR, 0, (PCWSTR)null);
            Append(menu, CmdClose, "Close this game", !closing);
            var pt = PointOf(lParam);
            PInvoke.ClientToScreen(_hwnd, &pt);
            int cmd = PInvoke.TrackPopupMenu(menu, TRACK_POPUP_MENU_FLAGS.TPM_RETURNCMD | TRACK_POPUP_MENU_FLAGS.TPM_RIGHTBUTTON,
                pt.X, pt.Y, 0, _hwnd, (RECT*)null);
            switch (cmd)
            {
                case CmdControl:
                    if (controlled)
                        Release("from the menu");
                    else
                        Control(g, null);
                    break;
                case CmdSound:
                    _monitor.SetAudio(g, !audio);
                    break;
                case CmdSolo:
                    _focusPid = _focusPid == g.Pid ? 0 : g.Pid;
                    Relayout();
                    break;
                case CmdLogs:
                    string logs = Path.Combine(instanceDir, "logs");
                    OpenFolder(Directory.Exists(logs) ? logs : instanceDir);
                    break;
                case CmdFolder:
                    OpenFolder(instanceDir);
                    break;
                case CmdCopyUrl:
                    CopyText(g.UrlWithToken);
                    break;
                case CmdClose:
                    _monitor.CloseGame(g);
                    break;
            }
        }
        finally
        {
            PInvoke.DestroyMenu(menu);
        }
    }

    private static void Append(HMENU menu, int id, string text, bool enabled, bool check = false)
    {
        var flags = MENU_ITEM_FLAGS.MF_STRING;
        if (!enabled)
            flags |= MENU_ITEM_FLAGS.MF_GRAYED;
        if (check)
            flags |= MENU_ITEM_FLAGS.MF_CHECKED;
        fixed (char* p = text)
            PInvoke.AppendMenu(menu, flags, (nuint)id, p);
    }

    private static void OpenFolder(string path)
    {
        try
        {
            if (path.Length > 0)
                Process.Start(new ProcessStartInfo(path) { UseShellExecute = true })?.Dispose();
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or FileNotFoundException)
        {
            ManagerLog.Write("open " + path + ": " + e.Message);
        }
    }

    private void CopyText(string text)
    {
        if (!PInvoke.OpenClipboard(_hwnd))
            return;
        try
        {
            PInvoke.EmptyClipboard();
            nuint bytes = (nuint)((text.Length + 1) * sizeof(char));
            var mem = PInvoke.GlobalAlloc(GLOBAL_ALLOC_FLAGS.GMEM_MOVEABLE, bytes);
            if (mem.IsNull)
                return;
            var dst = (char*)PInvoke.GlobalLock(mem);
            if (dst is null)
            {
                PInvoke.GlobalFree(mem);
                return;
            }
            text.AsSpan().CopyTo(new Span<char>(dst, text.Length));
            dst[text.Length] = '\0';
            PInvoke.GlobalUnlock(mem);
            // The clipboard owns the memory once this succeeds.
            if (PInvoke.SetClipboardData(CfUnicodeText, new HANDLE(mem.Value)).IsNull)
                PInvoke.GlobalFree(mem);
        }
        finally
        {
            PInvoke.CloseClipboard();
        }
    }

    // --- placement ---------------------------------------------------------------------------------

    // The window's last screen rectangle (saved while it was neither minimized nor maximized), if a
    // screen still shows it.
    private void RestorePlacement()
    {
        try
        {
            if (JsonNode.Parse(File.ReadAllText(SettingsFile)) is not JsonObject o ||
                !Int(o, "x", out int x) || !Int(o, "y", out int y) || !Int(o, "w", out int w) || !Int(o, "h", out int h))
                return;
            if (w < 100 || h < 60)
                return;
            var r = new RECT(x, y, x + w, y + h);
            if (PInvoke.MonitorFromRect(&r, MONITOR_FROM_FLAGS.MONITOR_DEFAULTTONULL).IsNull)
                return;
            PInvoke.SetWindowPos(_hwnd, default, x, y, w, h, SET_WINDOW_POS_FLAGS.SWP_NOZORDER | SET_WINDOW_POS_FLAGS.SWP_NOACTIVATE);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
        }
    }

    private static bool Int(JsonObject o, string key, out int value)
    {
        value = 0;
        return o[key] is JsonValue v && v.TryGetValue(out value);
    }

    private void SavePlacement()
    {
        if (PInvoke.IsIconic(_hwnd) || PInvoke.IsZoomed(_hwnd) || !PInvoke.GetWindowRect(_hwnd, out var r))
            return; // keep the last normal one
        try
        {
            Directory.CreateDirectory(Paths.Root);
            File.WriteAllText(SettingsFile, new JsonObject
            {
                ["x"] = r.left, ["y"] = r.top, ["w"] = r.right - r.left, ["h"] = r.bottom - r.top,
            }.ToJsonString());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }
}
