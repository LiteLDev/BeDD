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
using Windows.Win32.UI.WindowsAndMessaging;

namespace BeDD.Gui;

// The manager window, plain Win32: a header line, then a tile per game with a caption (dot, title,
// instance and pid, status) above a live DWM thumbnail of the game window. Double-click a tile to
// show only that game (again: all), right-click it for its menu. Closing the window closes every
// game (it asks first, unless the stop event asked).
[System.Runtime.Versioning.SupportedOSPlatform("windows10.0.15063")]
internal sealed unsafe class ManagerWindow
{
    private const uint WM_DESTROY = 0x0002, WM_SIZE = 0x0005, WM_PAINT = 0x000F, WM_CLOSE = 0x0010,
        WM_QUERYENDSESSION = 0x0011, WM_ERASEBKGND = 0x0014, WM_ENDSESSION = 0x0016, WM_GETMINMAXINFO = 0x0024,
        WM_TIMER = 0x0113, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONUP = 0x0205, WM_DPICHANGED = 0x02E0, WM_APP = 0x8000;
    private const uint WM_APP_CHANGED = WM_APP + 1, WM_APP_STOP = WM_APP + 2, WM_APP_EXIT = WM_APP + 3;
    private const uint DWM_TNP_RECTDESTINATION = 0x1, DWM_TNP_OPACITY = 0x4, DWM_TNP_VISIBLE = 0x8, DWM_TNP_SOURCECLIENTAREAONLY = 0x10;
    private const int IdcArrow = 32512, AppIconId = 32512, CfUnicodeText = 13;
    private const int CmdSolo = 1, CmdLogs = 2, CmdFolder = 3, CmdCopyUrl = 4, CmdClose = 5;

    private static readonly COLORREF Back = Rgb(24, 24, 27), CaptionBack = Rgb(39, 39, 42), BodyBack = Rgb(0, 0, 0);
    private static readonly COLORREF Text1 = Rgb(244, 244, 245), Text2 = Rgb(161, 161, 170);
    private static readonly COLORREF ActiveDot = Rgb(74, 222, 128), IdleDot = Rgb(113, 113, 122), BadDot = Rgb(248, 113, 113);

    private static ManagerWindow? s_current;
    // Kept alive for as long as the class is registered.
    private static readonly WNDPROC s_wndProc = WndProc;

    private readonly GameMonitor _monitor;
    private readonly HashSet<Game> _thumbed = [];
    private HWND _hwnd;
    private uint _dpi = 96;
    private HFONT _font, _bold, _small;
    private int _focusPid;
    private int _changePosted;
    private bool _closing;

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
            case WM_LBUTTONDBLCLK:
                if (GameAt(lParam) is { } solo)
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
                    ManagerLog.Write("windows is shutting down");
                    SavePlacement();
                    _monitor.CloseAll();
                }
                return true;
            case WM_DESTROY:
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
                count += " (showing one: double-click it to show all)";
            Text(dc, count, _bold, Text1, new Rectangle(S(12), 0, header.Width / 2 - S(12), header.Height), DRAW_TEXT_FORMAT.DT_LEFT);
            Text(dc, "Closing this window closes every game in it", _small, Text2,
                new Rectangle(header.Width / 2, 0, header.Width / 2 - S(12), header.Height), DRAW_TEXT_FORMAT.DT_RIGHT);
            if (n == 0)
            {
                var body = Rectangle.FromLTRB(0, header.Bottom, client.Width, client.Height);
                int line = S(20);
                int mid = body.Top + body.Height / 2;
                Text(dc, "Games started with BeDD show up here.", _font, Text2,
                    new Rectangle(0, mid - line, client.Width, line), DRAW_TEXT_FORMAT.DT_CENTER);
                Text(dc, "Double-click a game to enlarge it, right-click it for more.", _font, Text2,
                    new Rectangle(0, mid, client.Width, line), DRAW_TEXT_FORMAT.DT_CENTER);
                return;
            }
            foreach (var g in Visible())
            {
                if (g.Tile.IsEmpty)
                    continue;
                Fill(dc, g.Caption, CaptionBack);
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
            bool closing;
            string instanceDir = g.Endpoint.InstanceDir;
            lock (_monitor.Sync)
                closing = g.Closing;
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

    private static void Append(HMENU menu, int id, string text, bool enabled)
    {
        fixed (char* p = text)
            PInvoke.AppendMenu(menu, enabled ? MENU_ITEM_FLAGS.MF_STRING : MENU_ITEM_FLAGS.MF_STRING | MENU_ITEM_FLAGS.MF_GRAYED, (nuint)id, p);
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
