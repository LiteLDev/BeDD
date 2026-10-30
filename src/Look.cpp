// The `look` op (config gameHooks): turn the local player to an absolute yaw/pitch, in the degrees
// of player_auth_input (yaw 0 = +Z, 90 = -X; pitch -90 = straight up). The client turns itself:
// a ClientLevelTickEvent listener calls LocalPlayer::localPlayerTurn with what is left to turn, as
// the game does for the mouse, so the client sends the new rotation in its own player_auth_input.
// The pipe thread only waits (condition variable) until the rotation is within kTolerance.
#include "BeDD.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>

#include <nlohmann/json.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"

namespace bedd {

namespace {

using json = nlohmann::json;

constexpr float kTolerance = 0.5f; // degrees, per axis

// localPlayerTurn(delta) turns by degrees, 1:1 and independent of the mouse sensitivity: the
// rotation (Actor::mRot = {pitch, yaw}) becomes {pitch - delta.x, yaw + delta.y}, pitch clamped to
// +-89.9. It lands after the tick that called it, before the next one. Measured in trace mode
// ("trace look" lines); the loop corrects any residue anyway.
constexpr int   kMaxWait    = 2;     // ticks to wait for a turn to land before turning again
constexpr float kMaxPitch   = 89.9f; // the game's own clamp
constexpr int   kMaxTimeout = 60000; // ms: a look never holds a pipe thread longer

enum class State { Pending, Done, NotInLevel, Superseded };

struct Request {
    uint64_t gen{};
    float    yaw{}, pitch{};
    State    state = State::Done;
    float    gotYaw{}, gotPitch{};
    uint64_t tick{};
    int      turns{};
};

ll::event::ListenerPtr  gTick;
std::atomic<bool>       gActive{false}; // gTick is registered (read by the pipe threads)
std::mutex              gMutex;
std::condition_variable gCv;
Request                 gReq; // guarded by gMutex
// Client thread only.
bool gTurned = false; // a turn was issued and may not have landed yet
Vec2 gTurnedFrom{};   // the rotation it was issued from
int  gWaited = 0;

// -180 < result <= 180
float wrapDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d <= -180.0f) d += 360.0f;
    if (d > 180.0f) d -= 360.0f;
    return d;
}

void finish(uint64_t gen, State s, Vec2 rot) {
    uint64_t tick = worldPose().tick; // the `world` op's tick counter
    {
        std::lock_guard lock(gMutex);
        if (gReq.gen != gen || gReq.state != State::Pending) return;
        gReq.state    = s;
        gReq.gotYaw   = rot.y;
        gReq.gotPitch = rot.x;
        gReq.tick     = tick;
    }
    gCv.notify_all();
}

bool within(Vec2 rot, float yaw, float pitch) {
    return std::abs(wrapDegrees(yaw - rot.y)) < kTolerance && std::abs(pitch - rot.x) < kTolerance;
}

// On the client thread, once per client tick.
void onTick() {
    Request req;
    {
        std::lock_guard lock(gMutex);
        if (gReq.state != State::Pending) {
            gTurned = false;
            return;
        }
        req = gReq;
    }
    auto ci = ll::service::getClientInstance();
    if (!ci) return;
    LocalPlayer* p = ci->getLocalPlayer();
    if (!p) return finish(req.gen, State::NotInLevel, {});
    Vec2 rot = p->getRotation();
    if (within(rot, req.yaw, req.pitch)) return finish(req.gen, State::Done, rot);
    // The previous turn lands after the tick that asked for it: wait for it (a tick or two) rather
    // than turning twice.
    if (gTurned && rot.x == gTurnedFrom.x && rot.y == gTurnedFrom.y && ++gWaited <= kMaxWait) return;
    float dYaw   = wrapDegrees(req.yaw - rot.y);
    float dPitch = req.pitch - rot.x;
    Vec2  delta{-dPitch, dYaw};
    p->localPlayerTurn(delta);
    gTurned     = true;
    gTurnedFrom = rot;
    gWaited     = 0;
    {
        std::lock_guard lock(gMutex);
        if (gReq.gen == req.gen) ++gReq.turns;
    }
    if (options().trace)
        log().info(
            "trace look: target=({:.2f},{:.2f}) rot=({:.2f},{:.2f}) localPlayerTurn({:.2f},{:.2f}) (yaw,pitch in degrees)",
            req.yaw,
            req.pitch,
            rot.y,
            rot.x,
            delta.x,
            delta.y
        );
}

json error(char const* code, std::string const& msg) {
    return {
        {"ok",    false},
        {"code",  code },
        {"error", msg  }
    };
}

float number(json const& j, char const* key) {
    auto const& v = j.at(key);
    if (!v.is_number()) throw std::invalid_argument(std::string(key) + " must be a number");
    auto f = v.get<float>();
    if (!std::isfinite(f)) throw std::invalid_argument(std::string(key) + " must be finite");
    return f;
}
} // namespace

void startLook() {
    if (!options().gameHooks || gTick) return;
    gTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>(
        [](ll::event::ClientLevelTickEvent&) { onTick(); }
    );
    gActive = gTick != nullptr;
}

void stopLook() {
    gActive = false;
    if (gTick) ll::event::EventBus::getInstance().removeListener(gTick);
    gTick = nullptr;
    lookLevelExited();
}

void lookLevelExited() {
    {
        std::lock_guard lock(gMutex);
        if (gReq.state == State::Pending) gReq.state = State::NotInLevel;
    }
    gCv.notify_all();
}

json lookOp(json const& j) {
    if (!options().gameHooks) return disabledReply("look");
    WorldPose pose = worldPose();
    if (!j.contains("yaw") && !j.contains("pitch")) {
        json r{
            {"ok",      true        },
            {"inLevel", pose.inLevel},
            {"yaw",     nullptr     },
            {"pitch",   nullptr     },
            {"tick",    pose.tick   },
        };
        if (pose.inLevel) {
            r["yaw"]   = pose.yaw;
            r["pitch"] = pose.pitch;
        }
        return r;
    }
    float yaw = 0, pitch = 0;
    int   timeoutMs = 1000;
    try {
        yaw   = wrapDegrees(number(j, "yaw"));
        pitch = std::clamp(number(j, "pitch"), -kMaxPitch, kMaxPitch);
        if (j.contains("timeoutMs")) timeoutMs = std::clamp(static_cast<int>(number(j, "timeoutMs")), 0, kMaxTimeout);
    } catch (std::exception const& e) {
        return error("bad_args", std::string("look: ") + e.what() + " (needs yaw and pitch, in degrees)");
    }
    if (!pose.inLevel || !gActive) return error("not_in_level", "look: not in a world");

    std::unique_lock lock(gMutex);
    if (gReq.state == State::Pending) gReq.state = State::Superseded;
    gCv.notify_all();
    uint64_t gen = gReq.gen + 1;
    gReq         = Request{.gen = gen, .yaw = yaw, .pitch = pitch, .state = State::Pending};
    bool ended   = gCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return gReq.gen != gen || gReq.state != State::Pending;
    });
    if (gReq.gen != gen) return error("busy", "look: superseded by a newer look");
    Request req = gReq;
    if (!ended) gReq.state = State::Done; // stop turning: the caller gave up
    lock.unlock();
    switch (req.state) {
    case State::Done:
        if (ended)
            return {
                {"ok",    true        },
                {"yaw",   req.gotYaw  },
                {"pitch", req.gotPitch},
                {"tick",  req.tick    },
                {"turns", req.turns   },
            };
        break;
    case State::NotInLevel: return error("not_in_level", "look: left the world");
    case State::Superseded: return error("busy", "look: superseded by a newer look");
    case State::Pending: break;
    }
    WorldPose now = worldPose();
    json      r   = error("timeout", "look: the rotation did not reach the target within timeoutMs");
    r["yaw"]      = now.yaw;
    r["pitch"]    = now.pitch;
    r["turns"]    = req.turns;
    return r;
}

} // namespace bedd
