using System.Buffers;
using System.Collections.Concurrent;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BeDD.Core;

// Why a call or a connection failed: "connect" (the game refused or did not answer the handshake),
// "timeout" (no reply in time; the connection stays usable and a late reply is dropped) or "closed"
// (the connection went away before the reply).
public sealed class GameClientException(string code, string message, Exception? inner = null) : Exception(message, inner)
{
    public string Code { get; } = code;
}

// One WebSocket connection to one game or server (a GameEndpoint from Discovery): JSON requests
// `{id, op, ...}` answered by `{id, ok, ...}`, matched by id, so several calls may be in flight at
// once (the mod answers them in order, one connection at a time). Binary messages (tap frames after
// `{op:"tap", action:"subscribe"}`) and text messages that answer no pending call are handed to the
// Binary and Message events. Events run on the receive loop's thread: keep them short and marshal
// to a UI thread yourself. Closed fires exactly once, whoever closed the connection; pending calls
// then fail with GameClientException("closed").
//
//     await using var game = await GameClient.ConnectAsync(endpoint, TimeSpan.FromSeconds(10));
//     JsonObject pong = await game.CallAsync(new JsonObject { ["op"] = "ping" });
//     if (!GameClient.IsOk(pong)) ...
//
// NativeAOT-safe: System.Net.WebSockets and JsonNode only.
public sealed class GameClient : IAsyncDisposable, IDisposable
{
    // The mod's cap on one message; larger ones close the connection.
    public const int MaxMessageBytes = 16 << 20;

    public static readonly TimeSpan DefaultCallTimeout = TimeSpan.FromSeconds(10);

    private readonly ClientWebSocket _ws;
    private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonObject>> _pending = new();
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly CancellationTokenSource _stop = new();
    private long _nextId;
    private int _closed;
    private volatile bool _closingHere;
    private Task _receiveLoop = Task.CompletedTask;

    private GameClient(GameEndpoint endpoint, ClientWebSocket ws)
    {
        Endpoint = endpoint;
        _ws = ws;
    }

    public GameEndpoint Endpoint { get; }

    public bool IsOpen => Volatile.Read(ref _closed) == 0 && _ws.State == WebSocketState.Open;

    // Why the connection closed, once it has ("" while open).
    public string CloseReason { get; private set; } = "";

    // Raised once when the connection is gone, with the reason.
    public event Action<GameClient, string>? Closed;

    // A text message that answers no pending call (a reply that came after its call timed out, or
    // anything the game sends on its own).
    public event Action<GameClient, JsonObject>? Message;

    // A binary message (one tap frame each after a tap subscribe). The memory is only valid during
    // the call: copy what you keep.
    public event Action<GameClient, ReadOnlyMemory<byte>>? Binary;

    // Connects with the endpoint's token in the Authorization header. Throws GameClientException
    // ("connect") when the game cannot be reached or refuses the token within timeout.
    public static async Task<GameClient> ConnectAsync(GameEndpoint endpoint, TimeSpan timeout, CancellationToken cancel = default)
    {
        ArgumentNullException.ThrowIfNull(endpoint);
        if (!Uri.TryCreate(endpoint.Url, UriKind.Absolute, out var uri) || (uri.Scheme != "ws" && uri.Scheme != "wss"))
            throw new GameClientException("connect", $"'{endpoint.Url}' is not a WebSocket url");
        var ws = new ClientWebSocket();
        ws.Options.Proxy = null; // loopback; never a system proxy
        ws.Options.KeepAliveInterval = TimeSpan.FromSeconds(15);
        if (endpoint.Token.Length > 0)
            ws.Options.SetRequestHeader("Authorization", "Bearer " + endpoint.Token);
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        cts.CancelAfter(timeout);
        try
        {
            await ws.ConnectAsync(uri, cts.Token).ConfigureAwait(false);
        }
        catch (Exception e) when (e is WebSocketException or OperationCanceledException or IOException
                                      or System.Net.Http.HttpRequestException)
        {
            ws.Dispose();
            cancel.ThrowIfCancellationRequested();
            string why = e is OperationCanceledException ? $"no answer within {timeout.TotalSeconds:0.#} s" : e.Message;
            throw new GameClientException("connect", $"cannot connect to {endpoint.Url}: {why}", e);
        }
        var client = new GameClient(endpoint, ws);
        client._receiveLoop = Task.Run(client.ReceiveLoop);
        return client;
    }

    public static bool IsOk(JsonObject? reply) =>
        reply?["ok"] is JsonValue v && v.TryGetValue(out bool ok) && ok;

    // The reply's "error" text, or "" when there is none.
    public static string Error(JsonObject? reply) =>
        reply?["error"] is JsonValue v && v.TryGetValue(out string? s) ? s ?? "" : "";

    // Sends request with a fresh "id" (any id it had is replaced; the object is modified) and returns
    // the reply as it came, ok:false included. Throws GameClientException("timeout") when no reply
    // comes within timeout (default 10 s), ("closed") when the connection is or goes away.
    public async Task<JsonObject> CallAsync(JsonObject request, TimeSpan? timeout = null, CancellationToken cancel = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (!IsOpen)
            throw new GameClientException("closed", "the connection is closed" + (CloseReason.Length > 0 ? ": " + CloseReason : ""));
        long id = Interlocked.Increment(ref _nextId);
        request["id"] = id;
        var tcs = new TaskCompletionSource<JsonObject>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = tcs;
        try
        {
            // Closed between the check above and the registration: Fail did not see this call.
            if (Volatile.Read(ref _closed) != 0)
                throw new GameClientException("closed", "the connection is closed: " + CloseReason);
            byte[] bytes = Encoding.UTF8.GetBytes(request.ToJsonString());
            await SendAsync(bytes, WebSocketMessageType.Text, cancel).ConfigureAwait(false);
            var wait = timeout ?? DefaultCallTimeout;
            try
            {
                return await tcs.Task.WaitAsync(wait, cancel).ConfigureAwait(false);
            }
            catch (TimeoutException)
            {
                string op = request["op"] is JsonValue o && o.TryGetValue(out string? s) ? s ?? "?" : "?";
                throw new GameClientException("timeout", $"no reply to '{op}' within {wait.TotalSeconds:0.#} s");
            }
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    // CallAsync for a request that is just an op.
    public Task<JsonObject> CallAsync(string op, TimeSpan? timeout = null, CancellationToken cancel = default) =>
        CallAsync(new JsonObject { ["op"] = op }, timeout, cancel);

    // Sends a message without waiting for anything (e.g. a binary message).
    public async Task SendAsync(ReadOnlyMemory<byte> data, WebSocketMessageType type, CancellationToken cancel = default)
    {
        await _sendGate.WaitAsync(cancel).ConfigureAwait(false);
        try
        {
            await _ws.SendAsync(data, type, true, cancel).ConfigureAwait(false);
        }
        catch (Exception e) when (e is WebSocketException or IOException or ObjectDisposedException or InvalidOperationException)
        {
            Fail("send failed: " + e.Message);
            throw new GameClientException("closed", "the connection is closed: " + e.Message, e);
        }
        finally
        {
            _sendGate.Release();
        }
    }

    // A polite close (waits up to timeout for the game's close frame), then the socket is released.
    public async Task CloseAsync(TimeSpan? timeout = null)
    {
        _closingHere = true;
        if (_ws.State == WebSocketState.Open)
        {
            using var cts = new CancellationTokenSource(timeout ?? TimeSpan.FromSeconds(2));
            try
            {
                await SendCloseAsync("bye", cts.Token).ConfigureAwait(false);
                await _receiveLoop.WaitAsync(cts.Token).ConfigureAwait(false);
            }
            catch (Exception e) when (e is WebSocketException or IOException or OperationCanceledException
                                          or ObjectDisposedException or InvalidOperationException)
            {
            }
        }
        Fail("closed by this side");
    }

    public async ValueTask DisposeAsync()
    {
        await CloseAsync().ConfigureAwait(false);
        _ws.Dispose();
    }

    // Drops the connection at once (no close handshake).
    public void Dispose()
    {
        Fail("closed by this side");
        _ws.Dispose();
    }

    private async Task ReceiveLoop()
    {
        var buffer = new ArrayBufferWriter<byte>(64 * 1024);
        string reason = "the game closed the connection";
        try
        {
            while (true)
            {
                buffer.ResetWrittenCount();
                ValueWebSocketReceiveResult r;
                do
                {
                    var mem = buffer.GetMemory(16 * 1024);
                    r = await _ws.ReceiveAsync(mem, _stop.Token).ConfigureAwait(false);
                    buffer.Advance(r.Count);
                    if (buffer.WrittenCount > MaxMessageBytes)
                    {
                        reason = "a message is larger than 16 MiB";
                        _ws.Abort();
                        return;
                    }
                } while (!r.EndOfMessage && r.MessageType != WebSocketMessageType.Close);
                if (r.MessageType == WebSocketMessageType.Close)
                {
                    if (_closingHere)
                        reason = "closed by this side";
                    else if (_ws.CloseStatusDescription is { Length: > 0 } d)
                        reason += ": " + d;
                    if (_ws.State == WebSocketState.CloseReceived)
                    {
                        try
                        {
                            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(2));
                            await SendCloseAsync("", cts.Token).ConfigureAwait(false);
                        }
                        catch (Exception e) when (e is WebSocketException or IOException or OperationCanceledException
                                                      or ObjectDisposedException or InvalidOperationException)
                        {
                        }
                    }
                    return;
                }
                if (r.MessageType == WebSocketMessageType.Binary)
                {
                    Raise(() => Binary?.Invoke(this, buffer.WrittenMemory));
                    continue;
                }
                Dispatch(buffer.WrittenSpan);
            }
        }
        catch (Exception) when (_closingHere || _stop.IsCancellationRequested)
        {
            reason = "closed by this side";
        }
        catch (Exception e) when (e is WebSocketException or IOException or ObjectDisposedException or InvalidOperationException)
        {
            reason = "connection lost: " + e.Message;
        }
        catch (Exception e)
        {
            reason = "receive failed: " + e.Message;
        }
        finally
        {
            Fail(reason);
        }
    }

    private void Dispatch(ReadOnlySpan<byte> text)
    {
        JsonObject? msg;
        try
        {
            msg = JsonNode.Parse(text) as JsonObject;
        }
        catch (JsonException)
        {
            return;
        }
        if (msg is null)
            return;
        if (msg["id"] is JsonValue idv && TryLong(idv, out long id) && _pending.TryRemove(id, out var tcs))
        {
            tcs.TrySetResult(msg);
            return;
        }
        Raise(() => Message?.Invoke(this, msg));
    }

    // A subscriber's exception must not take the connection down (or the game: a manager connection
    // that drops closes it).
    private static void Raise(Action a)
    {
        try
        {
            a();
        }
        catch (Exception)
        {
        }
    }

    // The close frame goes through the send gate: ClientWebSocket allows one send at a time.
    private async Task SendCloseAsync(string description, CancellationToken cancel)
    {
        await _sendGate.WaitAsync(cancel).ConfigureAwait(false);
        try
        {
            await _ws.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, description, cancel).ConfigureAwait(false);
        }
        finally
        {
            _sendGate.Release();
        }
    }

    private static bool TryLong(JsonValue v, out long id)
    {
        if (v.TryGetValue(out id))
            return true;
        if (v.TryGetValue(out double d) && d == Math.Floor(d))
        {
            id = (long)d;
            return true;
        }
        if (v.TryGetValue(out JsonElement e) && e.ValueKind == JsonValueKind.Number && e.TryGetInt64(out id))
            return true;
        id = 0;
        return false;
    }

    // Marks the connection closed (once): pending calls fail, Closed fires.
    private void Fail(string reason)
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0)
            return;
        CloseReason = reason;
        try
        {
            _stop.Cancel();
        }
        catch (ObjectDisposedException)
        {
        }
        if (_ws.State is WebSocketState.Open or WebSocketState.Connecting or WebSocketState.CloseReceived)
            _ws.Abort();
        foreach (var (id, tcs) in _pending)
        {
            if (_pending.TryRemove(id, out _))
                tcs.TrySetException(new GameClientException("closed", "the connection closed: " + reason));
        }
        Raise(() => Closed?.Invoke(this, reason));
    }
}
