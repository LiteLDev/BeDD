using System.Drawing;
using System.Globalization;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Gui;

internal enum GameStatusKind
{
    Idle,
    Active,
    Bad,
}

// One game the manager shows. The fields above "UI thread only" are written by GameMonitor under
// its lock and read by the window under the same lock.
internal sealed class Game(GameEndpoint endpoint)
{
    public GameEndpoint Endpoint { get; } = endpoint;
    public int Pid => Endpoint.Pid;

    public GameClient? Client;
    public bool Connected;
    public bool Attached;
    public string AttachNote = "";
    public nint Hwnd;
    public Size ClientSize;
    public long Synthetic = -1;
    public int Clients;
    public int Failures;
    public DateTime Started = DateTime.Now;
    public DateTime LastInput = DateTime.MinValue;
    public bool Closing;
    // The game's sound plays on this PC (the user turned it on; the game reports "audio").
    public bool Audio;
    // The game holds the mouse (in a world): the pointer is hidden while the user controls it.
    public bool Grabbed;
    // The user controls this game (the window's keyboard and mouse go to it).
    public bool Controlled;
    public KillJob? Job;
    public string JobNote = "";

    // UI thread only
    public nint Thumb;
    public nint ThumbSource;
    public Size Source;
    public Rectangle Tile, Caption, Body;
    // Where the picture is, inside Body (empty without one).
    public Rectangle Dest;

    // The tile's name: the label the game was started with, else its instance.
    public string Title => Endpoint.Label.Length > 0 ? Endpoint.Label : Endpoint.Instance.Length > 0 ? Endpoint.Instance : "Minecraft";

    // What follows the title: the instance (when the title is the label) and the pid.
    public string Subtitle =>
        (Endpoint.Label.Length > 0 && Endpoint.Instance.Length > 0 ? Endpoint.Instance + "  ·  " : "") +
        "pid " + Pid.ToString(CultureInfo.InvariantCulture);

    // The WebSocket url with the token, ready for a test script.
    public string UrlWithToken =>
        Endpoint.Token.Length == 0 ? Endpoint.Url : Endpoint.Url + (Endpoint.Url.Contains('?') ? "&" : "?") + "token=" + Endpoint.Token;

    // Takes the window fields of a ping or state reply (hwnd, client {w,h}) and the input counter.
    public void Apply(JsonObject reply, DateTime now)
    {
        if (reply["hwnd"] is JsonValue h && h.TryGetValue(out long hwnd))
            Hwnd = (nint)hwnd;
        if (reply["client"] is JsonObject c && c["w"] is JsonValue w && c["h"] is JsonValue hh &&
            w.TryGetValue(out int cw) && hh.TryGetValue(out int ch))
            ClientSize = new Size(cw, ch);
        if (reply["audio"] is JsonValue a && a.TryGetValue(out bool audio))
            Audio = audio;
        if (reply["grabbed"] is JsonValue gr && gr.TryGetValue(out bool grabbed))
            Grabbed = grabbed;
        if (reply["clients"] is JsonValue n && n.TryGetValue(out int clients))
            Clients = clients;
        if (reply["stats"] is JsonObject stats && stats["synthetic"] is JsonValue s && s.TryGetValue(out long synthetic))
        {
            if (Synthetic >= 0 && synthetic != Synthetic)
                LastInput = now;
            Synthetic = synthetic;
        }
    }

    // The status line on the tile's right and the colour of its dot.
    public (string Text, GameStatusKind Kind) Status(DateTime now)
    {
        if (Closing)
            return ("closing", GameStatusKind.Bad);
        if (!Connected)
            return ("connecting", GameStatusKind.Idle);
        if (!Attached && AttachNote.Length > 0)
            return ("not attached", GameStatusKind.Bad);
        if (Failures >= 3)
            return ("not answering", GameStatusKind.Bad);
        if (Hwnd == 0)
            return ("starting", GameStatusKind.Idle);
        if (Controlled)
            return (Extras("controlled"), GameStatusKind.Active);
        if (LastInput != DateTime.MinValue && (now - LastInput).TotalSeconds < 5)
            return (Extras("input"), GameStatusKind.Active);
        string text = "idle " + Ago(now - (LastInput != DateTime.MinValue ? LastInput : Started));
        if (Clients > 1)
            text += "  ·  " + (Clients - 1).ToString(CultureInfo.InvariantCulture) + (Clients == 2 ? " client" : " clients");
        return (Extras(text), GameStatusKind.Idle);
    }

    private string Extras(string status) => Audio ? status + "  ·  sound on" : status;

    // A point of the picture (window client coordinates) in game client pixels; null outside it.
    public Point? ToGame(Point p)
    {
        if (Dest.Width <= 0 || Dest.Height <= 0 || !Dest.Contains(p))
            return null;
        var size = ClientSize.Width > 0 && ClientSize.Height > 0 ? ClientSize : Source;
        if (size.Width <= 0 || size.Height <= 0)
            return null;
        return new Point((int)((long)(p.X - Dest.Left) * size.Width / Dest.Width), (int)((long)(p.Y - Dest.Top) * size.Height / Dest.Height));
    }

    public static string Ago(TimeSpan d)
    {
        if (d < TimeSpan.Zero)
            d = TimeSpan.Zero;
        if (d.TotalSeconds < 60)
            return ((int)d.TotalSeconds).ToString(CultureInfo.InvariantCulture) + " s";
        if (d.TotalMinutes < 60)
            return ((int)d.TotalMinutes).ToString(CultureInfo.InvariantCulture) + " min";
        return ((int)d.TotalHours).ToString(CultureInfo.InvariantCulture) + " h " + d.Minutes.ToString(CultureInfo.InvariantCulture) + " min";
    }
}
