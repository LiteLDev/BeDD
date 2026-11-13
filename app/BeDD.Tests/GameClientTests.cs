using System.Net.WebSockets;
using System.Text;
using System.Text.Json.Nodes;
using BeDD.Core;

namespace BeDD.Tests;

public class GameClientTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    private static JsonObject? Echo(JsonObject req) => (string?)req["op"] switch
    {
        "ping" => new JsonObject { ["ok"] = true, ["protocol"] = 2, ["hwnd"] = 1234 },
        "echo" => new JsonObject { ["ok"] = true, ["value"] = req["value"]?.DeepClone() },
        "silent" => null,
        _ => new JsonObject { ["ok"] = false, ["error"] = "unknown op", ["code"] = "unknown_op" },
    };

    [Fact]
    public async Task CallsAreAnsweredByIdAndInParallel()
    {
        await using var server = new FakeGameServer(Echo);
        await using var client = await GameClient.ConnectAsync(server.Endpoint(), Wait, TestContext.Current.CancellationToken);
        var calls = Enumerable.Range(0, 20)
            .Select(i => client.CallAsync(new JsonObject { ["op"] = "echo", ["value"] = i }, Wait, TestContext.Current.CancellationToken))
            .ToArray();
        var replies = await Task.WhenAll(calls);
        for (int i = 0; i < replies.Length; i++)
        {
            Assert.True(GameClient.IsOk(replies[i]));
            Assert.Equal(i, (int?)replies[i]["value"]);
        }
        var ping = await client.CallAsync("ping", cancel: TestContext.Current.CancellationToken);
        Assert.Equal(1234, (int?)ping["hwnd"]);
    }

    [Fact]
    public async Task ErrorRepliesAreReturnedNotThrown()
    {
        await using var server = new FakeGameServer(Echo);
        await using var client = await GameClient.ConnectAsync(server.Endpoint(), Wait, TestContext.Current.CancellationToken);
        var r = await client.CallAsync("nope", Wait, TestContext.Current.CancellationToken);
        Assert.False(GameClient.IsOk(r));
        Assert.Equal("unknown op", GameClient.Error(r));
    }

    [Fact]
    public async Task AWrongTokenIsRefused()
    {
        await using var server = new FakeGameServer(Echo);
        var e = await Assert.ThrowsAsync<GameClientException>(() =>
            GameClient.ConnectAsync(server.Endpoint(token: new string('0', 64)), Wait, TestContext.Current.CancellationToken));
        Assert.Equal("connect", e.Code);
    }

    [Fact]
    public async Task ATimeoutLeavesTheConnectionUsableAndALateReplyGoesToMessage()
    {
        await using var server = new FakeGameServer(Echo);
        await using var client = await GameClient.ConnectAsync(server.Endpoint(), Wait, TestContext.Current.CancellationToken);
        var late = new TaskCompletionSource<JsonObject>();
        client.Message += (_, m) => late.TrySetResult(m);
        var request = new JsonObject { ["op"] = "silent" };
        var e = await Assert.ThrowsAsync<GameClientException>(() =>
            client.CallAsync(request, TimeSpan.FromMilliseconds(200), TestContext.Current.CancellationToken));
        Assert.Equal("timeout", e.Code);
        Assert.True(client.IsOpen);
        long id = (long)request["id"]!;
        await server.BroadcastAsync(Encoding.UTF8.GetBytes(new JsonObject { ["id"] = id, ["ok"] = true }.ToJsonString()), WebSocketMessageType.Text);
        var m = await late.Task.WaitAsync(Wait, TestContext.Current.CancellationToken);
        Assert.Equal(id, (long?)m["id"]);
        Assert.True(GameClient.IsOk(await client.CallAsync("ping", Wait, TestContext.Current.CancellationToken)));
    }

    [Fact]
    public async Task BinaryMessagesGoToTheBinaryEvent()
    {
        await using var server = new FakeGameServer(Echo);
        await using var client = await GameClient.ConnectAsync(server.Endpoint(), Wait, TestContext.Current.CancellationToken);
        var got = new TaskCompletionSource<byte[]>();
        client.Binary += (_, b) => got.TrySetResult(b.ToArray());
        await client.CallAsync("ping", Wait, TestContext.Current.CancellationToken); // connected on the server side
        await server.BroadcastAsync([1, 2, 3, 4], WebSocketMessageType.Binary);
        Assert.Equal(new byte[] { 1, 2, 3, 4 }, await got.Task.WaitAsync(Wait, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task ClosedFiresOnceAndFailsPendingCalls()
    {
        await using var server = new FakeGameServer(Echo);
        var client = await GameClient.ConnectAsync(server.Endpoint(), Wait, TestContext.Current.CancellationToken);
        int closed = 0;
        var reason = new TaskCompletionSource<string>();
        client.Closed += (_, why) =>
        {
            Interlocked.Increment(ref closed);
            reason.TrySetResult(why);
        };
        await client.CallAsync("ping", Wait, TestContext.Current.CancellationToken);
        var pending = client.CallAsync("silent", Wait, TestContext.Current.CancellationToken);
        await server.CloseAllAsync();
        var e = await Assert.ThrowsAsync<GameClientException>(() => pending);
        Assert.Equal("closed", e.Code);
        Assert.Contains("game closing", await reason.Task.WaitAsync(Wait, TestContext.Current.CancellationToken));
        Assert.False(client.IsOpen);
        var after = await Assert.ThrowsAsync<GameClientException>(() => client.CallAsync("ping", Wait, TestContext.Current.CancellationToken));
        Assert.Equal("closed", after.Code);
        await client.DisposeAsync();
        Assert.Equal(1, closed);
    }

    [Fact]
    public async Task ANonWebSocketUrlIsAConnectError()
    {
        var e = await Assert.ThrowsAsync<GameClientException>(() =>
            GameClient.ConnectAsync(new GameEndpoint { Pid = 1, Url = "http://127.0.0.1:1/v1" }, Wait, TestContext.Current.CancellationToken));
        Assert.Equal("connect", e.Code);
    }
}
