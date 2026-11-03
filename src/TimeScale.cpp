// `timescale` op and config `timeScale`: how fast the game runs. Singleplayer is a client and its
// in-process local server, each with its own Minecraft and sim timer (Timer: mTicksPerSecond,
// mTimeScale), so both are set together (the timer's scale, and Minecraft::setSimTimePause for
// pausing): the local server's on the server thread
// (ServerInstance::queueForServerThread), the client's on the client thread (a ClientInstance::update
// hook: frames go on while the sim is paused, ticks and LeviLamina's thread executors do not), and
// the op replies once both are applied. The timers catch up at most a few ticks per frame
// (Timer::advanceTime), so a high scale can fall short: the reply reports the tick rates measured
// over the last two seconds (ClientLevelTickEvent, ServerLevelTickEvent), not the requested ones.
//
// On a server of its own (client + server mode) BeDD runs inside the server too and leads: it sends
// every player a LevelEvent packet 3007 (SimTimeScale, mData = the scale's float bits; 0 = paused),
// and the hooked ClientNetworkHandler::handle(LevelEventPacket) applies it to the client here
// (unless the game already did). From then on a set answers code:"server_led"; on any other remote server it
// answers code:"remote", since nothing of the server can be changed from the client.
#include "BeDD.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/client/ClientStartJoinLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/server/ServerInstance.h"
#include "mc/util/Timer.h"
#include "mc/world/Minecraft.h"

namespace bedd {

namespace {

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr float kMinScale = 0.05f;
constexpr float kMaxScale = 20.0f;
constexpr auto  kWindow   = 2s; // tick rates are measured over this much of the recent past

// Tick timestamps of the last kWindow, for one side.
class TickRate {
public:
    void tick() {
        auto            now = Clock::now();
        std::lock_guard lock(mMutex);
        if (mSince == Clock::time_point{}) mSince = now;
        mTicks.push_back(now);
        prune(now);
    }
    void reset() {
        std::lock_guard lock(mMutex);
        mTicks.clear();
        mSince = {};
    }
    /// Ticks per real second; 0 once no tick came for the whole window (paused, or not ticking).
    [[nodiscard]] double rate() {
        auto            now = Clock::now();
        std::lock_guard lock(mMutex);
        prune(now);
        if (mSince == Clock::time_point{}) return 0;
        // Just after the first tick, over the time since then (else it would read low).
        auto span = std::chrono::duration<double>(std::min<Clock::duration>(kWindow, now - mSince)).count();
        return span < 0.25 ? 0 : static_cast<double>(mTicks.size()) / span;
    }

private:
    void prune(Clock::time_point now) {
        while (!mTicks.empty() && now - mTicks.front() > kWindow) mTicks.pop_front();
    }
    std::mutex                    mMutex;
    std::deque<Clock::time_point> mTicks;
    Clock::time_point             mSince{};
};

TickRate gClientRate;
TickRate gServerRate;

// What one side's Minecraft reports right after a set.
struct Readback {
    bool  ok     = false;
    float scale  = 1; // Timer::mTimeScale
    bool  paused = false;
    float ticksPerSecond = 20; // Timer::mTicksPerSecond
};

std::mutex  gMutex; // everything below
float       gScale  = 1;
bool        gPaused = false;
std::string gSource = "default";
bool        gInLevel   = false;
bool        gConfigDue = false; // apply config timeScale on the next client tick (just joined)
bool        gServerLed = false; // a 3007 came from a remote server since joining began
Readback    gClientBack;
Readback    gServerBack;
bool        gLoggedVanilla = false; // logged once what the game itself does with a 3007

ll::event::ListenerPtr gClientTick;
ll::event::ListenerPtr gServerTick;
ll::event::ListenerPtr gStartJoin;
ll::event::ListenerPtr gJoin;
ll::event::ListenerPtr gExit;
std::atomic<bool>      gLocal{false}; // the world joined last is this client's own (ClientStartJoinLevelEvent)
std::atomic<bool>      gPacketHooked{false};
std::atomic<bool>      gUpdateHooked{false};

// Work for the client thread, run after the next ClientInstance::update.
std::mutex                         gJobsMutex;
std::vector<std::function<void()>> gClientJobs; // guarded by gJobsMutex
std::atomic<bool>                  gClientJobsDue{false};
bool                               gClientJobsRun = false; // the update hook runs them (guarded by gJobsMutex)

json fail(std::string_view code, std::string error) {
    return {
        {"ok",    false           },
        {"code",  code            },
        {"error", std::move(error)}
    };
}

void runOnClient(std::function<void()> fn) {
    std::unique_lock lock(gJobsMutex);
    if (!gClientJobsRun) {
        lock.unlock();
        ll::thread::ClientThreadExecutor::getDefault().execute(std::move(fn)); // not while paused
        return;
    }
    gClientJobs.push_back(std::move(fn));
    gClientJobsDue = true;
}

// False when there is no local server to run it.
bool runOnServer(std::function<void()> fn) {
    auto si = ll::service::getServerInstance();
    if (!si) return false;
    si->queueForServerThread(std::move(fn));
    return true;
}

// The sim timer's scale is written directly. Minecraft::setSimTimeScale multiplies the current
// scale and caps it at 10 (1.26.51: 0.5 then 2 gave 1, 2 then 3 gave 6, 3 then 5 gave 10), so it is
// not idempotent and cannot reach 20.
void setScale(Minecraft& mc, float scale) {
    if (options().trace && mc.mSimTimer.mTimeScale != scale) log().info("timescale: timer {} -> {}", mc.mSimTimer.mTimeScale, scale);
    mc.mSimTimer.mTimeScale = scale;
}

// On the side's own thread: apply (either may be absent) and read back.
Readback applyTo(bool clientSide, std::optional<float> scale, std::optional<bool> paused) {
    Readback r;
    auto     mc = ll::service::getMinecraft(clientSide);
    if (!mc || (!clientSide && !mc->getLevel())) return r;
    if (scale) setScale(*mc, *scale);
    if (paused) mc->setSimTimePause(*paused);
    r.ok             = true;
    r.scale          = mc->mSimTimer.mTimeScale;
    r.paused         = mc->getSimPaused();
    r.ticksPerSecond = mc->mSimTimer.mTicksPerSecond;
    return r;
}

json readbackJson(Readback const& b) {
    if (!b.ok) return nullptr;
    return {
        {"scale",          b.scale         },
        {"paused",         b.paused        },
        {"ticksPerSecond", b.ticksPerSecond}
    };
}

// The probe reply, and the body of every successful set (gMutex held).
json stateJson(bool local) {
    return {
        {"ok",             true                                                    },
        {"scale",          gScale                                                  },
        {"paused",         gPaused                                                 },
        {"source",         gSource                                                 },
        {"tps",            {{"client", gClientRate.rate()}, {"server", local ? json(gServerRate.rate()) : json()}}},
        {"ticksPerSecond", gClientBack.ok ? gClientBack.ticksPerSecond : 20.0f     },
        {"effective",      {{"client", readbackJson(gClientBack)}, {"server", readbackJson(gServerBack)}}},
        {"local",          local                                                   },
        {"inLevel",        gInLevel                                                },
    };
}

// On the client thread, entering a world: the config scale on both sides (the server's in the
// background). Every world starts at it, 1 included, so nothing of an earlier world carries over.
void applyConfigNow() {
    float scale = std::clamp(options().timeScale, kMinScale, kMaxScale);
    if (!gLocal) {
        if (scale != 1.0f) log().warn("timeScale {}: not applied, the world is on a remote server", scale);
        return;
    }
    Readback c = applyTo(true, scale, false);
    {
        std::lock_guard lock(gMutex);
        gScale      = scale;
        gPaused     = false;
        gSource     = scale != 1.0f ? "config" : "default";
        gClientBack = c;
    }
    runOnServer([scale] {
        Readback        s = applyTo(false, scale, false);
        std::lock_guard lock(gMutex);
        gServerBack = s;
    });
    if (scale != 1.0f) log().info("timeScale {} applied on entering the world", scale);
}

void onClientTick() {
    gClientRate.tick();
    bool due;
    {
        std::lock_guard lock(gMutex);
        due        = gConfigDue;
        gConfigDue = false;
    }
    if (due) applyConfigNow();
}

// Joining starts (before any packet of the world comes) or the client left: nothing of the last world.
void resetState(bool local) {
    gLocal = local;
    gClientRate.reset();
    gServerRate.reset();
    std::lock_guard lock(gMutex);
    gInLevel    = false;
    gConfigDue  = false;
    gServerLed  = false;
    gScale      = 1;
    gPaused     = false;
    gSource     = "default";
    gServerBack = {};
}

// In the world. The state is not reset here: the server's scale may have come already.
void onJoin() {
    gClientRate.reset();
    gServerRate.reset();
    std::lock_guard lock(gMutex);
    gInLevel   = true;
    gConfigDue = true;
}

// Leaving a world resets the client to normal speed (its Minecraft outlives the world; the local
// server's does not).
void onExit() {
    resetState(false);
    runOnClient([] {
        Readback        c = applyTo(true, 1.0f, false);
        std::lock_guard lock(gMutex);
        gClientBack = c;
    });
}

// A 3007 from the server, on the client thread after the game has handled the packet. Scale 0: the
// server paused (the client keeps its scale for the resume).
void onServerScale(float scale, float before) {
    auto mc = ll::service::getMinecraft(true);
    if (!mc) return;
    bool  pause = scale == 0;
    float after = mc->mSimTimer.mTimeScale;
    bool  local = gLocal;
    {
        std::lock_guard lock(gMutex);
        if (!gLoggedVanilla && before != scale) {
            gLoggedVanilla = true;
            log().info("timescale: a SimTimeScale level event of {} took the client timer from {} to {} by itself", scale, before, after);
        }
    }
    if (pause) {
        if (after != before) setScale(*mc, before); // whatever the game made of a 0
        if (!mc->getSimPaused()) mc->setSimTimePause(true);
    } else {
        if (mc->getSimPaused()) mc->setSimTimePause(false);
        // Not fighting the game: set it only where it did not take the scale as given (it multiplies).
        if (after != scale) setScale(*mc, scale);
    }
    Readback        c{true, mc->mSimTimer.mTimeScale, mc->getSimPaused(), mc->mSimTimer.mTicksPerSecond};
    std::lock_guard lock(gMutex);
    gClientBack = c;
    // The local server's own broadcast (should it send one) only echoes what was set here.
    if (local) return;
    gServerLed = true;
    gPaused    = pause;
    if (!pause) gScale = scale;
    gSource = "server";
    if (options().trace) log().info("timescale: the server {}", pause ? "paused" : "set " + std::to_string(scale));
}

} // namespace

// Outside the anonymous namespace: the macros' static members would be unused there.
LL_TYPE_INSTANCE_HOOK(
    TimeScaleLevelEventHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    static_cast<void (ClientNetworkHandler::*)(NetworkIdentifier const&, LevelEventPacket const&)>(
        &ClientNetworkHandler::$handle
    ),
    void,
    NetworkIdentifier const& source,
    LevelEventPacket const&  packet
) {
    bool  scaleEvent = packet.mEventId == static_cast<int>(SharedTypes::Legacy::LevelEvent::SimTimeScale);
    float before     = 0;
    if (scaleEvent)
        if (auto mc = ll::service::getMinecraft(true)) before = mc->mSimTimer.mTimeScale;
    origin(source, packet);
    if (!scaleEvent) return;
    float scale = std::bit_cast<float>(static_cast<int32_t>(packet.mData));
    if (!std::isfinite(scale) || scale < 0) {
        log().warn("timescale: ignored a SimTimeScale level event with scale {}", scale);
        return;
    }
    onServerScale(scale, before);
}

LL_TYPE_INSTANCE_HOOK(TimeScaleClientUpdateHook, ll::memory::HookPriority::Normal, ClientInstance, &ClientInstance::$update, bool, bool isInitFinished) {
    bool r = origin(isInitFinished);
    if (gClientJobsDue.exchange(false)) {
        std::vector<std::function<void()>> jobs;
        {
            std::lock_guard lock(gJobsMutex);
            jobs.swap(gClientJobs);
        }
        for (auto& job : jobs) job();
    }
    return r;
}

void installTimeScale() {
    if (gClientTick) return;
    auto& bus   = ll::event::EventBus::getInstance();
    gClientTick = bus.emplaceListener<ll::event::ClientLevelTickEvent>([](ll::event::ClientLevelTickEvent&) { onClientTick(); });
    gServerTick = bus.emplaceListener<ll::event::ServerLevelTickEvent>([](ll::event::ServerLevelTickEvent&) { gServerRate.tick(); });
    gStartJoin  = bus.emplaceListener<ll::event::ClientStartJoinLevelEvent>([](ll::event::ClientStartJoinLevelEvent& ev) {
        resetState(ev.isJoiningLocalServer());
    });
    gJoin       = bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](ll::event::ClientJoinLevelEvent&) { onJoin(); });
    gExit       = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](ll::event::ClientExitLevelEvent&) { onExit(); });
    {
        HookPause pause;
        gPacketHooked = TimeScaleLevelEventHook::hook(false) == 0;
        gUpdateHooked = TimeScaleClientUpdateHook::hook(false) == 0;
    }
    {
        std::lock_guard lock(gJobsMutex);
        gClientJobsRun = gUpdateHooked;
    }
    if (!gPacketHooked) log().error("timescale: ClientNetworkHandler::handle(LevelEventPacket) could not be hooked: a server's scale is not followed");
    if (!gUpdateHooked) log().error("timescale: ClientInstance::update could not be hooked: a paused game cannot be resumed");
    if (!gClientTick || !gServerTick) log().error("timescale: a level tick listener could not be added; tick rates read 0");
}

void uninstallTimeScale() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto* l : {&gClientTick, &gServerTick, &gStartJoin, &gJoin, &gExit}) {
        if (*l) bus.removeListener(*l);
        *l = nullptr;
    }
    if (gPacketHooked || gUpdateHooked) {
        HookPause pause;
        if (gPacketHooked) TimeScaleLevelEventHook::unhook(false);
        if (gUpdateHooked) TimeScaleClientUpdateHook::unhook(false);
        gPacketHooked = false;
        gUpdateHooked = false;
    }
    // Jobs never run now: their promises break, and a waiting set answers at once.
    std::lock_guard lock(gJobsMutex);
    gClientJobsRun = false;
    gClientJobs.clear();
}

json timeScaleOp(json const& req) {
    bool hasScale  = req.contains("scale");
    bool hasPaused = req.contains("paused");
    bool local     = gLocal;
    if (!hasScale && !hasPaused) {
        std::lock_guard lock(gMutex);
        return stateJson(local);
    }
    if (hasScale && (!req["scale"].is_number() || !std::isfinite(req["scale"].get<double>()) || req["scale"].get<double>() <= 0))
        return fail("bad_args", "scale must be a number (0.05..20)");
    if (hasPaused && !req["paused"].is_boolean()) return fail("bad_args", "paused must be true or false");
    if (req.contains("timeoutMs") && !req["timeoutMs"].is_number_integer()) return fail("bad_args", "timeoutMs must be an integer");
    std::optional<float> scale;
    std::optional<bool>  paused;
    if (hasScale) scale = static_cast<float>(std::clamp(req["scale"].get<double>(), double{kMinScale}, double{kMaxScale}));
    if (hasPaused) paused = req["paused"].get<bool>();
    {
        std::lock_guard lock(gMutex);
        if (!gInLevel) return fail("not_in_level", "not in a world");
        if (!local && gServerLed) return fail("server_led", "the server sets the speed (BeDD on the server): use its pipe");
    }
    if (!local) return fail("remote", "the world is on a remote server: its speed cannot be changed from the client");

    auto timeoutMs = req.contains("timeoutMs") ? std::clamp<int64_t>(req["timeoutMs"].get<int64_t>(), 50, 60000) : 2000;
    auto deadline  = Clock::now() + std::chrono::milliseconds(timeoutMs);
    auto server    = std::make_shared<std::promise<Readback>>();
    auto client    = std::make_shared<std::promise<Readback>>();
    auto sf        = server->get_future();
    auto cf        = client->get_future();
    // Each side records what it applied itself, so a side that applies after the timeout still shows.
    auto job = [scale, paused](bool clientSide, std::shared_ptr<std::promise<Readback>> const& done) {
        Readback r = applyTo(clientSide, scale, paused);
        {
            std::lock_guard lock(gMutex);
            if (r.ok) (clientSide ? gClientBack : gServerBack) = r;
        }
        done->set_value(r);
    };
    {
        // The target from now on: a side that misses the timeout still applies it.
        std::lock_guard lock(gMutex);
        if (scale) gScale = *scale;
        if (paused) gPaused = *paused;
        gSource = "local";
    }
    if (!runOnServer([job, server] { job(false, server); })) return fail("remote", "the local server went away");
    runOnClient([job, client] { job(true, client); });
    bool sReady = sf.wait_until(deadline) == std::future_status::ready;
    bool cReady = cf.wait_until(deadline) == std::future_status::ready;
    if (!sReady || !cReady)
        return fail(
            "timeout",
            std::string("the ") + (!sReady && !cReady ? "server and client threads" : !sReady ? "server thread" : "client thread")
                + " did not apply it within the timeout (it does once it runs; see effective in the probe)"
        );
    Readback s, c;
    try {
        s = sf.get();
        c = cf.get();
    } catch (std::exception const& e) {
        return fail("refused", std::string("the game dropped the change: ") + e.what());
    }
    if (!c.ok) return fail("not_in_level", "the client has no Minecraft");
    if (!s.ok) return fail("remote", "the local server went away");
    std::lock_guard lock(gMutex);
    log().info("timescale: scale {} paused {} (server timer {}, client timer {})", gScale, gPaused, s.scale, c.scale);
    return stateJson(true);
}

} // namespace bedd
