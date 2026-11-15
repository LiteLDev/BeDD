using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Gui;

// Sends the user's keyboard and mouse to one game while they control it in the manager window:
// {op:"manager", action:"input", events:[...]} over the game's connection. Events queue up while a
// request is out and go together in the next one, in order; pointer moves and relative movement
// that follow each other are merged, so a fast mouse never builds a backlog. Thread-safe.
internal sealed class InputForwarder(GameClient client, Action<JsonObject>? replied = null)
{
    private static readonly TimeSpan CallTimeout = TimeSpan.FromSeconds(3);

    private readonly object _lock = new();
    private readonly List<JsonObject> _queue = [];
    private bool _sending;

    public GameClient Client { get; } = client;

    public void Key(int vk, bool down) => Add(new JsonObject { ["t"] = "key", ["vk"] = vk, ["down"] = down });

    public void Char(int c) => Add(new JsonObject { ["t"] = "char", ["c"] = c });

    public void Move(int x, int y) => Add(new JsonObject { ["t"] = "move", ["x"] = x, ["y"] = y });

    public void Raw(int dx, int dy)
    {
        if (dx != 0 || dy != 0)
            Add(new JsonObject { ["t"] = "raw", ["dx"] = dx, ["dy"] = dy });
    }

    public void Button(string button, bool down) => Add(new JsonObject { ["t"] = "button", ["button"] = button, ["down"] = down });

    public void Wheel(int notches)
    {
        if (notches != 0)
            Add(new JsonObject { ["t"] = "wheel", ["notches"] = notches });
    }

    // Lets go of every key and button this connection holds in the game.
    public void Release() => Add(new JsonObject { ["t"] = "release" });

    // The events waiting to go (tests).
    internal JsonObject[] Pending()
    {
        lock (_lock)
            return [.. _queue];
    }

    private void Add(JsonObject e)
    {
        lock (_lock)
        {
            if (!Merge(e))
                _queue.Add(e);
            if (_sending)
                return;
            _sending = true;
        }
        _ = Task.Run(Pump);
    }

    // A move replaces the move right before it; relative movement adds to the one right before it.
    private bool Merge(JsonObject e)
    {
        if (_queue.Count == 0)
            return false;
        var last = _queue[^1];
        string t = (string)e["t"]!;
        if (t != (string)last["t"]!)
            return false;
        if (t == "move")
        {
            _queue[^1] = e;
            return true;
        }
        if (t == "raw")
        {
            last["dx"] = (int)last["dx"]! + (int)e["dx"]!;
            last["dy"] = (int)last["dy"]! + (int)e["dy"]!;
            return true;
        }
        return false;
    }

    private async Task Pump()
    {
        while (true)
        {
            JsonArray events;
            lock (_lock)
            {
                if (_queue.Count == 0)
                {
                    _sending = false;
                    return;
                }
                events = new JsonArray([.. _queue.Cast<JsonNode>()]);
                _queue.Clear();
            }
            if (!Client.IsOpen)
                continue; // dropped: the game lets go of what this connection held
            try
            {
                var reply = await Client.CallAsync(new JsonObject { ["op"] = "manager", ["action"] = "input", ["events"] = events }, CallTimeout)
                    .ConfigureAwait(false);
                if (!GameClient.IsOk(reply))
                    ManagerLog.Write("input refused: " + GameClient.Error(reply));
                replied?.Invoke(reply);
            }
            catch (GameClientException e)
            {
                ManagerLog.Write("input: " + e.Message);
            }
        }
    }
}
