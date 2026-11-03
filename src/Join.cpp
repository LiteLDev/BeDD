// The `join` op: the client joins a server by address, through the game's own connect path
// (ClientInstance::connectToThirdPartyServer, the external-server join behind "Add server" ->
// "Play" and the minecraft://connect deep link), on the client thread. From inside a world the
// client leaves it first (requestLeaveGame) and connects once the leave is done.
//
// The pipe thread never touches the game: it hands the attempt to the client thread, where a
// ClientInstance::update hook (every frame, also on the title screen, where neither client ticks nor
// LeviLamina's ClientThreadExecutor run) steps it every kPoll until the client is in the level
// (ClientJoinLevelEvent), the join failed (ClientCancelJoinLevelEvent or a disconnect screen) or the
// attempt timed out. The reply comes once the connection attempt started, or with `wait:true` once
// it ended. One attempt at a time.
//
// The level listeners also tell `state` and the probe (no `host`) where the client plays: nowhere,
// a local world (ClientStartJoinLevelEvent::isJoiningLocalServer), or a remote host:port (the
// client's GameConnectionInfo).
#include "BeDD.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <nlohmann/json.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientCancelJoinLevelEvent.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/client/ClientStartJoinLevelEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/network/DisconnectionScreenParams.h"
#include "mc/network/GameConnectionInfo.h"
#include "mc/network/connection/DisconnectFailReason.h"

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto kPoll   = 100ms;
constexpr auto kSettle = 2s; // after a leave, before the connect

struct Attempt {
    std::string       host;
    int               port = 19132;
    Clock::time_point deadline;
    bool              leaving  = false; // requestLeaveGame was called
    bool              left     = false; // a world was left first
    Clock::time_point leftAt;           // when the leave was requested
    bool              started  = false; // connectToThirdPartyServer was called
    bool              done     = false; // joined, failed or timed out: `result` is the reply
    json              result;
};

std::mutex              gMutex; // everything below
std::condition_variable gChanged;
std::shared_ptr<Attempt> gAttempt; // the attempt in progress (nullptr: none)
bool                     gStopping = false;
bool                     gInLevel  = false;
bool                     gLocal    = false; // the last join was to a local world
std::string              gHost;            // remote: the server's address
int                      gPort = 0;
json                     gLastResult; // the last finished attempt's reply
Clock::time_point        gNextStep;   // when the frame hook steps gAttempt next
std::atomic<bool>        gUpdateHooked{false}, gDisconnectHooked{false};

ll::event::ListenerPtr gStartListener, gJoinListener, gCancelListener, gExitListener;

json fail(std::string_view code, std::string error) {
    return {
        {"ok",    false           },
        {"code",  code            },
        {"error", std::move(error)}
    };
}

// The attempt ends with `reply` (gMutex held). By value: `a` may be gAttempt, which this resets.
void finish(std::shared_ptr<Attempt> a, json reply) {
    if (a->done) return;
    a->done   = true;
    a->result = std::move(reply);
    a->result["host"] = a->host;
    a->result["port"] = a->port;
    a->result["left"] = a->left;
    gLastResult       = a->result;
    if (gAttempt == a) gAttempt = nullptr;
    if (options().trace) log().info("join {}:{}: {}", a->host, a->port, a->result.dump());
    gChanged.notify_all();
}

// On the client thread, every kPoll until the attempt is done.
void step(std::shared_ptr<Attempt> const& a) {
    auto        ci  = ll::service::getClientInstance();
    std::string top = ci ? ci->getTopScreenName() : std::string();
    enum class Do { Nothing, Leave, Connect, Check } todo = Do::Nothing;
    {
        std::lock_guard lock(gMutex);
        if (a->done) return;
        if (gStopping) return finish(a, fail("stopped", "BeDD is unloading"));
        if (Clock::now() >= a->deadline) {
            // The game may still be connecting (`screen`): a later join or the probe shows how it ended.
            json r = fail("timeout", a->started ? "the client did not get into the level within the timeout" : "the client did not get to connect within the timeout");
            r["screen"] = top;
            return finish(a, std::move(r));
        }
        if (!ci) return finish(a, fail("no_client", "the client instance does not exist (yet)"));
        if (a->started) todo = Do::Check;
        else if (gInLevel) todo = a->leaving ? Do::Nothing : Do::Leave;
        else todo = Do::Connect;
    }
    // The engine calls run without gMutex: they may raise the level events synchronously.
    switch (todo) {
    case Do::Leave:
        if (options().trace) log().info("join {}:{}: leaving the current world first", a->host, a->port);
        ci->requestLeaveGame(true, false);
        {
            std::lock_guard lock(gMutex);
            a->leaving = a->left = true;
            a->leftAt            = Clock::now();
        }
        break;
    case Do::Nothing:
        break;
    case Do::Connect: {
        // Not while a progress screen shows (an earlier join still connecting, a world still loading
        // or saving), and after a leave once the game is back in its menus: a connect while it still
        // tears the world down hangs on "joining multiplayer external server".
        if (top.find("progress") != std::string::npos) break;
        if (a->leaving && (!ci->isLeaveGameDone() || Clock::now() < a->leftAt + kSettle)) break;
        log().info("join: connecting to {}:{}", a->host, a->port);
        ci->connectToThirdPartyServer(a->host, a->port);
        std::lock_guard lock(gMutex);
        a->started = true;
        gChanged.notify_all();
        break;
    }
    case Do::Check: {
        // Without the disconnection hooks: a failed connection ends on the classic disconnect screen
        // (with them, a disconnect screen left from an earlier join must not fail this one).
        std::lock_guard lock(gMutex);
        if (!gDisconnectHooked && !gInLevel && top.find("disconnect") != std::string::npos) {
            finish(a, json{{"ok", false}, {"code", "join_failed"}, {"error", "the client shows the disconnect screen"}, {"screen", top}});
            return;
        }
        break;
    }
    }
}

// The client flagged a disconnection (reason: Connection::DisconnectFailReason, -1 = not known).
void disconnected(int reason) {
    std::lock_guard lock(gMutex);
    if (options().trace) log().info("join: the client flagged a disconnection (reason {})", reason);
    if (!gAttempt || !gAttempt->started || gInLevel) return;
    json r = fail("join_failed", "the client could not connect (it shows why)");
    if (reason >= 0) r["reason"] = reason;
    finish(gAttempt, std::move(r));
}

// On the client thread, every frame: step the attempt in progress every kPoll.
void frame() {
    std::shared_ptr<Attempt> a;
    {
        std::lock_guard lock(gMutex);
        if (!gAttempt || Clock::now() < gNextStep) return;
        a         = gAttempt;
        gNextStep = Clock::now() + kPoll;
    }
    step(a);
}

// The game can reach `host`: loopbackOnly is off, or it is on the PC (localhost, 127.x, ::1), or in
// allowHosts (as written there; a name is not resolved here).
bool reachable(std::string host) {
    if (!options().loopbackOnly) return true;
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (host == "localhost" || host == "::1" || host == "[::1]" || host.starts_with("127.")) return true;
    return std::ranges::any_of(options().allowHosts, [&](std::string h) {
        for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return h == host;
    });
}

std::string connectionName() {
    if (!gInLevel) return "none";
    return gLocal ? "local" : "remote";
}

// {inLevel, connection, host, port} (gMutex held).
json connectionLocked() {
    json r{
        {"inLevel",    gInLevel         },
        {"connection", connectionName() },
    };
    if (gInLevel && !gLocal) {
        r["host"] = gHost;
        r["port"] = gPort;
    }
    return r;
}

} // namespace

// Outside the anonymous namespace: the macro's static members would be unused there.
LL_TYPE_INSTANCE_HOOK(JoinUpdateHook, ll::memory::HookPriority::Normal, ClientInstance, &ClientInstance::$update, bool, bool isInitFinished) {
    bool r = origin(isInitFinished);
    if (isInitFinished) frame();
    return r;
}

// The client gives up a connection (and shows why): the attempt in progress failed.
LL_TYPE_INSTANCE_HOOK(
    JoinDisconnectHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$flagDisconnectionAndNotify,
    void,
    ::Connection::DisconnectFailReason reason
) {
    disconnected(static_cast<int>(reason));
    origin(reason);
}

LL_TYPE_INSTANCE_HOOK(
    JoinDisconnectParamsHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$flagDisconnectionAndNotifyWithParams,
    void,
    ::DisconnectionScreenParams const& params
) {
    disconnected(-1);
    origin(params);
}

void installJoin() {
    if (gJoinListener) return;
    {
        std::lock_guard lock(gMutex);
        gStopping = false;
    }
    auto& bus       = ll::event::EventBus::getInstance();
    gStartListener  = bus.emplaceListener<ll::event::ClientStartJoinLevelEvent>([](ll::event::ClientStartJoinLevelEvent& e) {
        std::lock_guard lock(gMutex);
        gLocal = e.isJoiningLocalServer();
    });
    gJoinListener = bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](ll::event::ClientJoinLevelEvent& e) {
        auto conn = e.self().getGameConnectionInfo();
        std::lock_guard lock(gMutex);
        gInLevel = true;
        gHost.clear();
        gPort = 0;
        if (conn) {
            // A local world is a Local connection (to 127.0.0.1:0); ClientStartJoinLevelEvent is the fallback.
            gLocal = conn->mType == ::Social::ConnectionType::Local;
            gHost  = conn->mHostIpAddress.get().empty() ? conn->mUnresolvedUrl.get() : conn->mHostIpAddress.get();
            gPort  = conn->mPort;
        }
        if (options().trace)
            log().info("joined a level: local={} connection type={} host={} port={}", gLocal, conn ? static_cast<int>(conn->mType) : -1, gHost, gPort);
        if (gAttempt && gAttempt->started) finish(gAttempt, json{{"ok", true}, {"joined", true}, {"connection", connectionName()}});
    });
    gCancelListener = bus.emplaceListener<ll::event::ClientCancelJoinLevelEvent>([](ll::event::ClientCancelJoinLevelEvent&) {
        std::lock_guard lock(gMutex);
        if (gAttempt && gAttempt->started) finish(gAttempt, fail("join_failed", "the client cancelled the join"));
    });
    gExitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](ll::event::ClientExitLevelEvent&) {
        std::lock_guard lock(gMutex);
        gInLevel = false;
        gLocal   = false;
    });
    {
        HookPause pause;
        gUpdateHooked     = JoinUpdateHook::hook(false) == 0;
        gDisconnectHooked = JoinDisconnectHook::hook(false) == 0 && JoinDisconnectParamsHook::hook(false) == 0;
    }
    if (!gUpdateHooked) log().error("join: ClientInstance::update could not be hooked: the `join` op is unavailable");
    if (!gDisconnectHooked) log().warn("join: the disconnection hooks failed: a failed join ends at its timeout");
    log().info("join: mode {}{}", options().mode, gJoinListener ? "" : " (level listeners failed: `join` cannot tell when the client is in)");
}

void uninstallJoin() {
    {
        std::lock_guard lock(gMutex);
        gStopping = true;
        if (gAttempt) finish(gAttempt, fail("stopped", "BeDD is unloading"));
    }
    auto& bus = ll::event::EventBus::getInstance();
    for (auto* l : {&gStartListener, &gJoinListener, &gCancelListener, &gExitListener}) {
        if (*l) bus.removeListener(*l);
        *l = nullptr;
    }
    HookPause pause;
    // unhook() of a hook that is not in is a no-op.
    JoinUpdateHook::unhook(false);
    JoinDisconnectHook::unhook(false);
    JoinDisconnectParamsHook::unhook(false);
    gUpdateHooked = gDisconnectHooked = false;
}

json connectionInfo() {
    std::lock_guard lock(gMutex);
    return connectionLocked();
}

json joinOp(json const& req) {
    if (!req.contains("host")) {
        std::lock_guard lock(gMutex);
        json            r = connectionLocked();
        r["ok"]           = true;
        r["mode"]         = options().mode;
        r["joining"]      = gAttempt ? json{{"host", gAttempt->host}, {"port", gAttempt->port}, {"started", gAttempt->started}} : json(nullptr);
        r["last"]         = gLastResult;
        return r;
    }
    if (!req["host"].is_string() || req["host"].get<std::string>().empty()) return fail("bad_args", "host: a host name or address");
    auto a  = std::make_shared<Attempt>();
    a->host = req["host"].get<std::string>();
    if (req.contains("port")) {
        if (!req["port"].is_number_integer() || req["port"].get<int64_t>() < 1 || req["port"].get<int64_t>() > 65535)
            return fail("bad_args", "port: 1..65535");
        a->port = req["port"].get<int>();
    }
    if (!reachable(a->host))
        return fail("not_allowed", a->host + " is off the PC and not in allowHosts (loopbackOnly): use Remote mode (Start-BeDDGame.ps1 -Mode Remote -ServerHost)");
    bool wait    = req.value("wait", false);
    auto timeout = std::chrono::milliseconds(std::clamp(req.value("timeoutMs", 60000), 100, 600000));
    a->deadline  = Clock::now() + timeout;

    std::unique_lock lock(gMutex);
    if (gAttempt) return fail("busy", "a join to " + gAttempt->host + ":" + std::to_string(gAttempt->port) + " is in progress");
    if (gStopping) return fail("stopped", "BeDD is unloading");
    if (!gUpdateHooked) return fail("unavailable", "ClientInstance::update could not be hooked (see the log)");
    gAttempt  = a;
    gNextStep = Clock::now();
    // The client thread ends the attempt at its deadline; should it not run, the pipe gives up a bit later.
    auto giveUp = [&] {
        finish(a, fail("timeout", "the client thread did not run within the timeout (game busy?)"));
        return a->result;
    };
    // Started, or done (a failure before it started, or a quick join).
    if (!gChanged.wait_until(lock, a->deadline + 2s, [&] { return a->started || a->done; })) return giveUp();
    if (a->done) return a->result;
    if (!wait) return json{{"ok", true}, {"started", true}, {"host", a->host}, {"port", a->port}, {"left", a->left}};
    if (!gChanged.wait_until(lock, a->deadline + 2s, [&] { return a->done; })) return giveUp();
    return a->result;
}

} // namespace bedd
