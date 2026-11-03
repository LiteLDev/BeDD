// BeDD inside a Bedrock Dedicated Server (target_type=server): the `server`, `timescale` and
// `command` ops of the client+server debug mode.
//
// The server leads the game speed. `timescale` sets the server's own on the server thread: the scale
// straight into the simulation timer (Minecraft::mSimTimer), since Minecraft::setSimTimeScale
// multiplies the current scale and caps the product at 10, and the pause with setSimTimePause. It
// broadcasts the absolute scale to every player as a LevelEvent packet, event 3007 (SimTimeScale)
// with the float's bits as data, 0 while paused; a player who joins later gets it once it has
// spawned (PlayerJoinEvent). BeDD on the client follows that event. Config timeScale is applied once the
// server has started (LeviLamina enables mods only after that: then right away).
//
// A ServerLevelTickEvent listener counts the ticks the server really runs: the `server` op and the
// `timescale` probe report that measured rate (the timer may not catch up at a high scale), next to
// the rate the scale asks for.
//
// Work for the server thread goes through ServerInstance::queueForServerThread: LeviLamina's
// ServerThreadExecutor runs nothing while the simulation is paused, so a resume could not get through.
#include "BeDD.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/GamingStatus.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
#include "mc/locale/I18n.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/server/ServerInstance.h"
#include "mc/server/ServerLevel.h"
#include "mc/server/ServerPlayer.h"
#include "mc/server/commands/Command.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandOutputType.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CurrentCmdVersion.h"
#include "mc/server/commands/MinecraftCommands.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/util/Timer.h"
#include "mc/world/Minecraft.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"

namespace bedd {

namespace {

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr double kMinScale = 0.05;
constexpr double kMaxScale = 20.0;
// The measured tick rate is the ticks of the last two seconds.
constexpr auto kTpsWindow = 2s;

// Who set the scale the server runs at: nobody (1.0), config timeScale, or a `timescale` op.
enum class Source : int { Default, Config, Local };

ll::event::ListenerPtr gTickListener;
ll::event::ListenerPtr gStartedListener;
ll::event::ListenerPtr gJoinListener;
ll::event::ListenerPtr gLeaveListener;
std::atomic<int>       gSource{static_cast<int>(Source::Default)};
std::atomic<uint64_t>  gTick{};          // the level's current tick, as of its last tick
std::atomic<bool>      gConfigApplied{}; // config timeScale was looked at (once)

std::mutex             gTpsMutex;
std::deque<Clock::time_point> gTickTimes; // guarded by gTpsMutex: the ticks of the last kTpsWindow
Clock::time_point      gFirstTick{};      // guarded by gTpsMutex

struct PlayerEntry {
    std::string name, xuid;
};
std::mutex               gPlayersMutex;
std::vector<PlayerEntry> gPlayers; // guarded by gPlayersMutex: who joined and has not left

char const* sourceName(int s) {
    switch (static_cast<Source>(s)) {
    case Source::Config: return "config";
    case Source::Local: return "local";
    default: return "default";
    }
}

json fail(std::string code, std::string const& error) {
    return {
        {"ok",    false          },
        {"code",  std::move(code)},
        {"error", error          }
    };
}

void dropOldTicks(Clock::time_point now) {
    while (!gTickTimes.empty() && now - gTickTimes.front() > kTpsWindow) gTickTimes.pop_front();
}

// On the server thread, every level tick.
void onTick(ll::event::ServerLevelTickEvent& ev) {
    gTick     = ev.level().getCurrentTick().tickID;
    auto now  = Clock::now();
    std::lock_guard lock(gTpsMutex);
    if (gFirstTick == Clock::time_point{}) gFirstTick = now;
    gTickTimes.push_back(now);
    dropOldTicks(now);
}

// Ticks per second over the last kTpsWindow (less right after the first tick), one decimal.
double measuredTps() {
    auto            now = Clock::now();
    std::lock_guard lock(gTpsMutex);
    dropOldTicks(now);
    if (gFirstTick == Clock::time_point{}) return 0;
    double window = std::chrono::duration<double>(std::min<Clock::duration>(kTpsWindow, now - gFirstTick)).count();
    if (window < 0.25) return 0;
    return std::round(static_cast<double>(gTickTimes.size()) / window * 10) / 10;
}

// LevelEvent 3007 (SimTimeScale) carrying `scale`: to one player, or to every player (nullptr).
// On the server thread.
void sendScale(float scale, Player const* to) {
    std::shared_ptr<Packet> packet = MinecraftPackets::createPacket(MinecraftPacketIds::LevelEvent);
    if (!packet) return;
    auto& event    = static_cast<LevelEventPacket&>(*packet);
    event.mEventId = static_cast<int>(SharedTypes::Legacy::LevelEvent::SimTimeScale);
    event.mPos     = Vec3{0, 0, 0};
    event.mData    = std::bit_cast<int32_t>(scale);
    if (to) packet->sendTo(*to);
    else packet->sendToClients();
}

// On the server thread: run the scale and/or the pause.
bool setTimer(std::optional<float> scale, std::optional<bool> paused) {
    auto mc = ll::service::getMinecraft();
    if (!mc) return false;
    if (scale) mc->mSimTimer.mTimeScale = *scale; // not setSimTimeScale: it multiplies (see above)
    if (paused) mc->setSimTimePause(*paused);
    return true;
}

// What the players are told: the scale the server runs at, 0 while it is paused.
float announcedScale(Minecraft& mc) {
    float scale = mc.mSimTimer.mTimeScale;
    return mc.getSimPaused() ? 0.0f : scale;
}

// On the server thread: tell the players.
void broadcastScale() {
    if (auto mc = ll::service::getMinecraft()) sendScale(announcedScale(*mc), nullptr);
}

// On the server thread: set, then tell the players.
bool apply(std::optional<float> scale, std::optional<bool> paused) {
    if (!setTimer(scale, paused)) return false;
    broadcastScale();
    return true;
}

// Runs fn on the server thread and waits for its result; nullopt if it did not run within timeout
// (it still runs later). fn returns json.
template <class F>
std::optional<json> onServerThread(F fn, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<json>>();
    auto result  = promise->get_future();
    auto server  = ll::service::getServerInstance();
    if (!server) return json(fail("not_in_level", "the server has no ServerInstance"));
    server->queueForServerThread([promise, fn = std::move(fn)]() mutable {
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    if (result.wait_for(timeout) != std::future_status::ready) return std::nullopt;
    return result.get();
}

// timeoutMs of a request (1..120000 ms); nullopt if it is there but not a number.
std::optional<std::chrono::milliseconds> timeoutOf(json const& req, int defaultMs) {
    auto it = req.find("timeoutMs");
    if (it == req.end()) return std::chrono::milliseconds(defaultMs);
    if (!it->is_number()) return std::nullopt;
    return std::chrono::milliseconds(static_cast<int>(std::clamp(it->get<double>(), 1.0, 120000.0)));
}

// The server's port (server.properties, in the server's working directory); 0 if unknown.
int serverPort() {
    std::ifstream in("server.properties");
    std::string   line;
    while (std::getline(in, line)) {
        if (line.rfind("server-port=", 0) != 0) continue;
        try {
            return std::stoi(line.substr(12));
        } catch (std::exception const&) {
            return 0;
        }
    }
    return 0;
}

// The `timescale` probe: what the server runs now.
bool started() { return ll::getGamingStatus() == ll::GamingStatus::Running && ll::service::getMinecraft(); }

json timeScaleState() {
    auto mc = ll::service::getMinecraft();
    if (!mc || !started()) return fail("not_in_level", "the server has not started yet");
    Timer const& timer  = mc->mSimTimer;
    bool         paused = mc->getSimPaused();
    float        scale  = timer.mTimeScale;
    return {
        {"ok",             true                                        },
        {"scale",          scale                                       },
        {"paused",         paused                                      },
        {"source",         sourceName(gSource)                         },
        {"tps",            {{"client", nullptr}, {"server", measuredTps()}}},
        // The rate the scale asks for; tps.server is the rate the server achieves.
        {"ticksPerSecond", paused ? 0.0f : timer.mTicksPerSecond * scale},
    };
}

// Translated lines of a command's output.
json outputLines(CommandOutput const& output) {
    json  lines  = json::array();
    auto  locale = getI18n().getCurrentLanguage();
    for (auto const& m : output.mMessages) lines.push_back(getI18n().get(m.mMessageId, m.mParams, locale));
    return lines;
}

// On the server thread, once the server has started: config timeScale.
void applyConfig() {
    if (gConfigApplied.exchange(true)) return;
    float scale = options().timeScale;
    if (scale == 1.0f) return;
    if (!(scale >= static_cast<float>(kMinScale) && scale <= static_cast<float>(kMaxScale))) {
        log().warn("config timeScale {} is not in {}..{}; ignored", scale, kMinScale, kMaxScale);
        return;
    }
    if (apply(scale, std::nullopt)) {
        gSource = static_cast<int>(Source::Config);
        log().info("time scale {} (config timeScale)", scale);
    }
}

} // namespace

void startServerTimeScale() {
    auto& bus        = ll::event::EventBus::getInstance();
    gTickListener    = bus.emplaceListener<ll::event::ServerLevelTickEvent>(onTick);
    gStartedListener = bus.emplaceListener<ll::event::ServerStartedEvent>([](ll::event::ServerStartedEvent&) { applyConfig(); });
    // LeviLamina enables mods once the server has started, after ServerStartedEvent (and before it
    // reports GamingStatus::Running): the first server thread task applies it then.
    if (auto server = ll::service::getServerInstance()) server->queueForServerThread(applyConfig);
    // After spawn (LeviLamina fires it when the player is initialized): the client is ready to follow.
    gJoinListener = bus.emplaceListener<ll::event::PlayerJoinEvent>([](ll::event::PlayerJoinEvent& ev) {
        Player& p = ev.self();
        {
            std::lock_guard lock(gPlayersMutex);
            gPlayers.push_back({p.getRealName(), p.getXuid()});
        }
        auto mc = ll::service::getMinecraft();
        if (!mc) return;
        float scale = announcedScale(*mc);
        sendScale(scale, &p);
        if (options().trace) log().info("time scale {} sent to {}", scale, p.getRealName());
    });
    gLeaveListener = bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& ev) {
        std::string     name = ev.self().getRealName();
        std::lock_guard lock(gPlayersMutex);
        // The oldest entry of that name: one who rejoined before the old session left stays listed.
        if (auto it = std::ranges::find(gPlayers, name, &PlayerEntry::name); it != gPlayers.end()) gPlayers.erase(it);
    });
}

void stopServerTimeScale() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto* l : {&gTickListener, &gStartedListener, &gJoinListener, &gLeaveListener}) {
        if (*l) bus.removeListener(*l);
        l->reset();
    }
}

json serverInfoOp(json const&) {
    if (!started()) return fail("not_in_level", "the server has not started yet");
    json players = json::array();
    {
        std::lock_guard lock(gPlayersMutex);
        for (auto const& p : gPlayers)
            players.push_back({
                {"name", p.name},
                {"xuid", p.xuid},
            });
    }
    return {
        {"ok",      true                                       },
        {"target",  "server"                                   },
        {"tick",    gTick.load()                               },
        {"tps",     measuredTps()                              },
        {"paused",  ll::service::getMinecraft()->getSimPaused()},
        {"port",    serverPort()                               },
        {"players", std::move(players)                         },
    };
}

json serverTimeScaleOp(json const& req) {
    std::optional<float> scale;
    std::optional<bool>  paused;
    if (auto it = req.find("scale"); it != req.end()) {
        if (!it->is_number()) return fail("bad_args", "scale must be a number");
        double s = it->get<double>();
        if (!(s >= kMinScale && s <= kMaxScale)) return fail("bad_args", "scale must be within 0.05..20");
        scale = static_cast<float>(s);
    }
    if (auto it = req.find("paused"); it != req.end()) {
        if (!it->is_boolean()) return fail("bad_args", "paused must be true or false");
        paused = it->get<bool>();
    }
    auto timeout = timeoutOf(req, 5000);
    if (!timeout) return fail("bad_args", "timeoutMs must be a number");
    if (!scale && !paused) return timeScaleState();
    if (!started()) return fail("not_in_level", "the server has not started yet");
    auto r = onServerThread([=] { return json(apply(scale, paused)); }, *timeout);
    if (!r) return fail("timeout", "the server thread did not apply the scale within the timeout");
    if (r->is_object()) return *r; // no ServerInstance
    if (!r->get<bool>()) return fail("not_in_level", "the server has no Minecraft instance");
    if (scale) gSource = static_cast<int>(Source::Local);
    if (options().trace)
        log().info("timescale: scale={} paused={}", scale ? std::to_string(*scale) : "-", paused ? (*paused ? "true" : "false") : "-");
    return timeScaleState();
}

json serverCommandOp(json const& req) {
    auto it = req.find("command");
    if (it == req.end() || !it->is_string() || it->get<std::string>().empty())
        return fail("bad_args", "command needs a 'command' string");
    std::string command = it->get<std::string>();
    if (command.front() == '/') command.erase(0, 1);
    auto timeout = timeoutOf(req, 10000);
    if (!timeout) return fail("bad_args", "timeoutMs must be a number");
    if (!started()) return fail("not_in_level", "the server has not started yet");
    auto r = onServerThread(
        [command] {
            auto mc    = ll::service::getMinecraft();
            auto level = ll::service::getLevel();
            if (!mc || !level || !mc->mCommands) return fail("not_in_level", "the server has no level");
            // As the server console would run it.
            ServerCommandOrigin origin("Server", level->asServer(), CommandPermissionLevel::Owner, DimensionType(0));
            std::string         parseError;
            // Owned by MinecraftCommands (its compiled command cache, by command text): not freed here.
            Command*            compiled = mc->mCommands->compileCommand(
                HashedString(command),
                origin,
                CurrentCmdVersion::Latest,
                [&](std::string const& e) { parseError = e; }
            );
            if (!compiled) return fail("bad_args", parseError.empty() ? "the command does not parse" : parseError);
            CommandOutput output(CommandOutputType::AllOutput);
            compiled->run(origin, output);            return json{
                {"ok",           true                     },
                {"success",      output.mSuccessCount > 0 },
                {"successCount", output.mSuccessCount     },
                {"output",       outputLines(output)      },
            };
        },
        *timeout
    );
    if (!r) return fail("timeout", "the server thread did not run the command within the timeout");
    return *r;
}

} // namespace bedd
