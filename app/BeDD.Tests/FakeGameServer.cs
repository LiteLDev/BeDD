using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Tests;

// A stand-in for the mod's WebSocket side: ws://127.0.0.1:<port>/v1, the token checked like the mod
// does (Authorization: Bearer or ?token=), each text request answered by Handler (null = no reply).
internal sealed class FakeGameServer : IAsyncDisposable
{
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource _stop = new();
    private readonly List<WebSocket> _sockets = [];
    private readonly Task _accept;

    public FakeGameServer(Func<JsonObject, JsonObject?> handler)
    {
        Handler = handler;
        _listener.Start();
        _accept = Task.Run(AcceptLoop);
    }

    public Func<JsonObject, JsonObject?> Handler { get; set; }
    public string Token { get; } = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(32));
    public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;
    public string Url => $"ws://127.0.0.1:{Port}/v1";
    public int Connections;

    public GameEndpoint Endpoint(int pid = 0, string? token = null) => new()
    {
        Protocol = 2,
        Kind = "client",
        Pid = pid == 0 ? Environment.ProcessId : pid,
        Url = Url,
        Token = token ?? Token,
        Instance = "base-9",
        InstanceDir = @"C:\nowhere\base-9",
        Label = "test",
        StartedAt = DateTimeOffset.Now,
    };

    // Sends a message to every connection on the server's own initiative.
    public async Task BroadcastAsync(byte[] data, WebSocketMessageType type)
    {
        WebSocket[] all;
        lock (_sockets)
            all = [.. _sockets];
        foreach (var ws in all)
            await ws.SendAsync(data, type, true, default);
    }

    // Closes every connection (as a game that exits).
    public async Task CloseAllAsync()
    {
        WebSocket[] all;
        lock (_sockets)
            all = [.. _sockets];
        foreach (var ws in all)
        {
            try
            {
                await ws.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "game closing", default);
            }
            catch (Exception e) when (e is WebSocketException or IOException or ObjectDisposedException)
            {
            }
        }
    }

    private async Task AcceptLoop()
    {
        while (!_stop.IsCancellationRequested)
        {
            TcpClient tcp;
            try
            {
                tcp = await _listener.AcceptTcpClientAsync(_stop.Token);
            }
            catch (Exception e) when (e is OperationCanceledException or SocketException or ObjectDisposedException)
            {
                return;
            }
            _ = Task.Run(() => Serve(tcp));
        }
    }

    private async Task Serve(TcpClient tcp)
    {
        using var _ = tcp;
        var stream = tcp.GetStream();
        var head = new StringBuilder();
        var one = new byte[1];
        while (!head.ToString().EndsWith("\r\n\r\n", StringComparison.Ordinal))
        {
            if (await stream.ReadAsync(one) == 0)
                return;
            head.Append((char)one[0]);
        }
        var lines = head.ToString().Split("\r\n");
        string target = lines[0].Split(' ')[1];
        var headers = lines.Skip(1).Where(l => l.Contains(':'))
            .ToDictionary(l => l[..l.IndexOf(':')].Trim(), l => l[(l.IndexOf(':') + 1)..].Trim(), StringComparer.OrdinalIgnoreCase);
        bool authorized = (headers.TryGetValue("Authorization", out var auth) && auth == "Bearer " + Token) ||
                          target.EndsWith("token=" + Token, StringComparison.Ordinal);
        if (!target.StartsWith("/v1", StringComparison.Ordinal) || !authorized || headers.ContainsKey("Origin"))
        {
            await stream.WriteAsync(Encoding.ASCII.GetBytes("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
            return;
        }
        string accept = Convert.ToBase64String(SHA1.HashData(Encoding.ASCII.GetBytes(headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
        await stream.WriteAsync(Encoding.ASCII.GetBytes(
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n"));
        using var ws = WebSocket.CreateFromStream(stream, new WebSocketCreationOptions { IsServer = true });
        Interlocked.Increment(ref Connections);
        lock (_sockets)
            _sockets.Add(ws);
        var buffer = new byte[1 << 16];
        try
        {
            while (ws.State == WebSocketState.Open)
            {
                var r = await ws.ReceiveAsync(buffer, _stop.Token);
                if (r.MessageType == WebSocketMessageType.Close)
                {
                    await ws.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "", default);
                    return;
                }
                if (JsonNode.Parse(buffer.AsSpan(0, r.Count)) is not JsonObject req)
                    continue;
                var reply = Handler(req);
                if (reply is null)
                    continue;
                reply["id"] = req["id"]?.DeepClone();
                await ws.SendAsync(Encoding.UTF8.GetBytes(reply.ToJsonString()), WebSocketMessageType.Text, true, default);
            }
        }
        catch (Exception e) when (e is WebSocketException or IOException or OperationCanceledException or ObjectDisposedException)
        {
        }
        finally
        {
            lock (_sockets)
                _sockets.Remove(ws);
        }
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        _listener.Stop();
        try
        {
            await _accept;
        }
        catch (OperationCanceledException)
        {
        }
    }
}
