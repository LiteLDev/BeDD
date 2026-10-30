// The `world` op (config gameHooks): what the client shows and where its player is, so a pipe client
// (bedboo's router) can tell whether input reaches the world. A ClientLevelTickEvent listener takes
// the player's pose on the client thread every tick, a ClientInstance::update hook the screen state
// every frame; the op only copies the last snapshot and never touches the game from the pipe thread.
#include "BeDD.h"

#include <atomic>
#include <mutex>

#include <nlohmann/json.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/input/InputMode.h"
#include "mc/legacy/ActorRuntimeID.h"

namespace bedd {

namespace {

using json = nlohmann::json;

struct Snapshot {
    bool        inLevel = false;
    uint64_t    levelEpoch{};
    std::string topScreen;
    bool        mouseGrabbed = false;
    bool        showingMenu  = false;
    InputMode   inputMode    = InputMode::Undefined;
    bool        hasPlayer    = false;
    uint64_t    runtimeId{};
    Vec3        eye{};
    Vec2        rot{}; // x = pitch, y = yaw (degrees, as in player_auth_input)
    int         slot{};
    uint64_t    tick{};
};

ll::event::ListenerPtr gTick;
ll::event::ListenerPtr gJoin;
ll::event::ListenerPtr gExit;
std::atomic<bool>      gUpdateHooked{false}; // ClientInstance::update is hooked (screen fields every frame)

// Held for a copy only: the client thread never waits on the pipe.
std::mutex gMutex;
Snapshot   gSnap; // guarded by gMutex

char const* inputModeName(InputMode m) {
    switch (m) {
    case InputMode::Mouse: return "mouse";
    case InputMode::Touch: return "touch";
    case InputMode::GamePad: return "gamepad";
    case InputMode::MotionControllerDeprecated: return "motionController";
    default: return "undefined";
    }
}

// On the client thread: what the client shows. Every frame (the tick stops while a singleplayer
// game is paused, and outside worlds), and every tick in case the frame hook is missing.
void takeScreen(ClientInstance& ci) {
    std::string top     = ci.getTopScreenName();
    bool        grabbed = ci.getMouseGrabbed();
    bool        menu    = ci.isShowingMenu();
    InputMode   mode    = ci.getCurrentInputMode();
    std::lock_guard lock(gMutex);
    if (gSnap.topScreen != top) gSnap.topScreen = std::move(top);
    gSnap.mouseGrabbed = grabbed;
    gSnap.showingMenu  = menu;
    gSnap.inputMode    = mode;
}

// On the client thread, every client tick: the local player.
void takePose() {
    auto ci = ll::service::getClientInstance();
    if (!ci) return;
    if (!gUpdateHooked) takeScreen(*ci);
    {
        // Not between ClientExitLevelEvent and the next join: the player may be torn down.
        std::lock_guard lock(gMutex);
        if (!gSnap.inLevel) return;
    }
    Snapshot s;
    if (LocalPlayer* p = ci->getLocalPlayer()) {
        s.hasPlayer = true;
        s.runtimeId = p->getRuntimeID().rawID;
        s.eye       = p->getEyePos();
        s.rot       = p->getRotation();
        s.slot      = p->getSelectedItemSlot();
    }
    std::lock_guard lock(gMutex);
    gSnap.hasPlayer = s.hasPlayer;
    gSnap.runtimeId = s.runtimeId;
    gSnap.eye       = s.eye;
    gSnap.rot       = s.rot;
    gSnap.slot      = s.slot;
    ++gSnap.tick;
}

} // namespace

// Outside the anonymous namespace: the macro's static members would be unused there.
LL_TYPE_INSTANCE_HOOK(ClientUpdateHook, ll::memory::HookPriority::Normal, ClientInstance, &ClientInstance::$update, bool, bool isInitFinished) {
    bool r = origin(isInitFinished);
    if (isInitFinished) takeScreen(*this);
    return r;
}


void startGameState() {
    if (!options().gameHooks || gTick) return;
    auto& bus = ll::event::EventBus::getInstance();
    gTick     = bus.emplaceListener<ll::event::ClientLevelTickEvent>([](ll::event::ClientLevelTickEvent&) {
        takePose();
    });
    gJoin = bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](ll::event::ClientJoinLevelEvent&) {
        std::lock_guard lock(gMutex);
        gSnap.inLevel = true;
        ++gSnap.levelEpoch;
    });
    gExit = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](ll::event::ClientExitLevelEvent&) {
        {
            std::lock_guard lock(gMutex);
            gSnap.inLevel   = false;
            gSnap.hasPlayer = false;
        }
        lookLevelExited();
    });
    {
        HookPause pause;
        gUpdateHooked = ClientUpdateHook::hook(false) == 0;
    }
    if (!gUpdateHooked) log().error("ClientInstance::update could not be hooked: `world` screen fields update only on client ticks");
}

void stopGameState() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto* l : {&gTick, &gJoin, &gExit}) {
        if (*l) bus.removeListener(*l);
        *l = nullptr;
    }
    if (gUpdateHooked) {
        HookPause pause;
        ClientUpdateHook::unhook(false);
        gUpdateHooked = false;
    }
}
WorldPose worldPose() {
    std::lock_guard lock(gMutex);
    return {gSnap.inLevel && gSnap.hasPlayer, gSnap.rot.y, gSnap.rot.x, gSnap.tick};
}

json disabledReply(char const* op) {
    return {
        {"ok",    false                                                         },
        {"code",  "disabled"                                                    },
        {"key",   "gameHooks"                                                   },
        {"error", std::string(op) + " is off (config.json \"gameHooks\": true)"}
    };
}

json worldOp(json const&) {
    if (!options().gameHooks) return disabledReply("world");
    Snapshot s;
    {
        std::lock_guard lock(gMutex);
        s = gSnap;
    }
    json r{
        {"ok",           true                       },
        {"inLevel",      s.inLevel                  },
        {"levelEpoch",   s.levelEpoch               },
        {"topScreen",    s.topScreen                },
        {"mouseGrabbed", s.mouseGrabbed             },
        {"showingMenu",  s.showingMenu              },
        {"inputMode",    inputModeName(s.inputMode) },
        {"tick",         s.tick                     },
        {"player",       nullptr                    },
    };
    if (s.inLevel && s.hasPlayer) {
        r["player"] = {
            {"runtimeId", s.runtimeId                                     },
            {"eye",       {{"x", s.eye.x}, {"y", s.eye.y}, {"z", s.eye.z}}},
            {"rot",       {{"yaw", s.rot.y}, {"pitch", s.rot.x}}          },
            {"slot",      s.slot                                          },
        };
    }
    return r;
}

} // namespace bedd
