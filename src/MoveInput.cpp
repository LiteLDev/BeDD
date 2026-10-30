// The "input" op: move, jump, sneak and sprint the local player inside the game's own input
// pipeline, so the client builds every player_auth_input itself (no forged packets, no keymap).
//
// An override {move x/z, jump, sneak, sprint} lives until its dead-man deadline (ttlMs, default
// 300 ms): a controller that stops refreshing it (crashed, hung, disconnected) never leaves the
// player running. "release", ttlMs:0 and a control-client disconnect clear it at once.
//
// Two post-hooks on the client's ECS input systems apply it, once per client tick:
//   * ClientInputUpdateSystem::extractRawHIDInput fills RawMoveInputComponent from the device
//     state: mRawInput (MoveInputState flags) and mRawMove are replaced by the override's.
//   * ClientInputUpdateSystem::inputHandlerUpdatePlayerState turns MoveInputComponent's input
//     state into the player's move vector and sneak/sprint/jump flags: its mInputState and
//     mRawInputState are replaced before the original runs (so it derives everything from the
//     override) and again after it (so the systems after it see the same state). The keyboard
//     handler changes those flags only on key events, so on the first tick without the override
//     they are let go of once; otherwise the player would keep running.
// Both apply only while the mouse is grabbed and no screen is showing, so a menu or chat opened on
// the client is never walked through.
//
// If either hook does not install, "input" falls back to the virtual keys of the default keymap
// (W/A/S/D, SPACE, LSHIFT, LCTRL) and reports via:'keys'; a timer thread lets them go at the deadline.
// Everything here is off unless config.json has "gameHooks": true.
#include "BeDD.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>

#include <nlohmann/json.hpp>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/entity/systems/ClientInputUpdateSystem.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/entity/components/MoveInputComponent.h"
#include "mc/entity/components/RawMoveInputComponent.h"
#include "mc/input/MoveInputState.h"

namespace bedd {

namespace {

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Flag  = MoveInputState::Flag;

struct Override {
    float             x = 0, z = 0; // player_auth_input move_vector: x = left - right, z = forward - back
    bool              jump = false, sneak = false, sprint = false;
    Clock::time_point deadline{};
    bool              active = false;
    uint64_t          owner  = 0; // the control-pipe client that set it (currentPipeClient(); 0 = manager)
};

std::mutex gMutex;
Override   gOverride; // guarded by gMutex

bool                  gInstalled = false;
std::atomic<bool>     gRawHooked{false}, gStateHooked{false};
std::atomic<uint64_t> gTicks{}, gApplied{};

constexpr float   kDead     = 0.3f;  // a stick past this counts as the matching direction key
constexpr int64_t kMaxTtlMs = 60000; // longer requests are cut to this: the switch stays a dead-man one

bool viaHooks() { return gRawHooked.load() && gStateHooked.load(); }

// The live override, or nothing once it expired (the dead-man switch).
std::optional<Override> live() {
    std::lock_guard lock(gMutex);
    if (!gOverride.active) return std::nullopt;
    if (Clock::now() >= gOverride.deadline) {
        gOverride.active = false;
        return std::nullopt;
    }
    return gOverride;
}

// In-world input only: never while a screen (pause menu, chat, inventory, ...) has the input.
bool inWorldInput() {
    auto ci = ll::service::getClientInstance();
    return ci && ci->getMouseGrabbed() && !ci->isShowingMenu();
}

// ---------------------------------------------------------------------------------------------
// Hooks (client thread)
// ---------------------------------------------------------------------------------------------
bool gPrevJump = false, gPrevSneak = false; // last state written, for the pressed/released edges

void writeState(MoveInputState& s, Override const& o) {
    auto& f = s.mFlagValues.get();
    for (Flag c :
         {Flag::SneakDown,
          Flag::SneakToggleDown,
          Flag::JumpDown,
          Flag::SprintDown,
          Flag::UpLeft,
          Flag::UpRight,
          Flag::DownLeft,
          Flag::DownRight,
          Flag::Up,
          Flag::Down,
          Flag::Left,
          Flag::Right,
          Flag::SneakInputCurrentlyDown,
          Flag::SneakInputWasReleased,
          Flag::SneakInputWasPressed,
          Flag::JumpInputWasReleased,
          Flag::JumpInputWasPressed,
          Flag::JumpInputCurrentlyDown})
        f.set(static_cast<size_t>(c), false);
    auto set = [&](Flag c, bool v) { f.set(static_cast<size_t>(c), v); };
    set(Flag::Up, o.z > kDead);
    set(Flag::Down, o.z < -kDead);
    set(Flag::Left, o.x > kDead);
    set(Flag::Right, o.x < -kDead);
    set(Flag::JumpDown, o.jump);
    set(Flag::JumpInputCurrentlyDown, o.jump);
    set(Flag::JumpInputWasPressed, o.jump && !gPrevJump);
    set(Flag::JumpInputWasReleased, !o.jump && gPrevJump);
    set(Flag::SneakDown, o.sneak);
    set(Flag::SneakInputCurrentlyDown, o.sneak);
    set(Flag::SneakInputWasPressed, o.sneak && !gPrevSneak);
    set(Flag::SneakInputWasReleased, !o.sneak && gPrevSneak);
    set(Flag::SprintDown, o.sprint);
    s.mAnalogMoveVector = Vec2{o.x, o.z};
}

// Like a keyboard: a diagonal is no faster than a straight line.
Override normalized(Override o) {
    float len = std::sqrt(o.x * o.x + o.z * o.z);
    if (len > 1.0f) {
        o.x /= len;
        o.z /= len;
    }
    return o;
}

// The override to apply on this tick, if any.
std::optional<Override> toApply() {
    auto o = live();
    if (!o || !inWorldInput()) return std::nullopt;
    return normalized(*o);
}

bool gWritten = false; // the last tick wrote the override (client thread)

LL_TYPE_STATIC_HOOK(
    ExtractRawHidHook,
    ll::memory::HookPriority::Normal,
    ClientInputUpdateSystem,
    &ClientInputUpdateSystem::extractRawHIDInput,
    void,
    ::MovementAbilitiesComponent const&         abilities,
    ::MoveInputComponent const&                 moveInput,
    ::ActorDataFlagComponent const&             synchedActorData,
    ::RawMoveInputComponent&                    rawMoveInput,
    ::Optional<::SneakingComponent const>       sneaking,
    ::Optional<::WasInWaterFlagComponent const> isInWater
) {
    origin(abilities, moveInput, synchedActorData, rawMoveInput, sneaking, isInWater);
    auto o = toApply();
    if (!o) return;
    writeState(rawMoveInput.mRawInput.get(), *o);
    rawMoveInput.mRawMove = Vec2{o->x, o->z};
}

LL_TYPE_STATIC_HOOK(
    UpdatePlayerStateHook,
    ll::memory::HookPriority::Normal,
    ClientInputUpdateSystem,
    &ClientInputUpdateSystem::inputHandlerUpdatePlayerState,
    void,
    ::MovementAbilitiesComponent const&         abilities,
    ::MobEffectsComponent const&                mobEffects,
    ::ActorDataFlagComponent const&             synchedActorData,
    ::ActorOwnerComponent&                      actorOwner,
    ::MoveInputComponent&                       moveInput,
    ::Optional<::PassengerComponent const>      isRiding,
    ::Optional<::WasInWaterFlagComponent const> isInWater
) {
    ++gTicks;
    auto o = toApply();
    if (!o && gWritten) {
        // The keyboard handler only changes these flags on key events, so what the override set
        // would stay (the player kept running after the deadline): let go of everything once.
        writeState(moveInput.mInputState.get(), Override{});
        writeState(moveInput.mRawInputState.get(), Override{});
        gPrevJump = gPrevSneak = false;
        gWritten  = false;
    }
    if (o) {
        writeState(moveInput.mInputState.get(), *o);
        writeState(moveInput.mRawInputState.get(), *o);
    }
    origin(abilities, mobEffects, synchedActorData, actorOwner, moveInput, isRiding, isInWater);
    if (!o) return;
    gWritten = true;
    writeState(moveInput.mInputState.get(), *o);
    writeState(moveInput.mRawInputState.get(), *o);
    gPrevJump  = o->jump;
    gPrevSneak = o->sneak;
    ++gApplied;
}

// ---------------------------------------------------------------------------------------------
// Key fallback (default keymap; any thread)
// ---------------------------------------------------------------------------------------------
struct MoveKey {
    int vk;
    int messageVk; // what WM_KEYDOWN carries
};
constexpr std::array<MoveKey, 7> kKeys{
    {{'W', 'W'}, {'S', 'S'}, {'A', 'A'}, {'D', 'D'}, {VK_SPACE, VK_SPACE}, {VK_LSHIFT, VK_SHIFT}, {VK_LCONTROL, VK_CONTROL}}
};

// Held so that the deadline check and the key changes are one step (a refresh never loses its keys).
std::mutex          gKeyMutex;
std::array<bool, 7> gKeysHeld{}; // guarded by gKeyMutex: keys this file pressed
std::jthread        gKeyTimer;

void postMoveKey(MoveKey k, bool down) {
    UINT   sc = MapVirtualKeyW(static_cast<UINT>(k.vk), MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (static_cast<LPARAM>(sc & 0xFF) << 16);
    if (!down) lp |= (1LL << 30) | (1LL << 31);
    UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
    post(Event{.msg = msg, .wp = static_cast<WPARAM>(k.messageVk), .lp = lp, .vk = k.vk, .vkDown = down});
}

// Hold exactly the keys of `o` (none when nullptr); gKeyMutex held. A key someone else holds
// down (the "key" op) is left alone: neither pressed twice nor released by us.
void applyKeysLocked(Override const* o) {
    std::array<bool, 7> want{};
    if (o) want = {o->z > kDead, o->z < -kDead, o->x > kDead, o->x < -kDead, o->jump, o->sneak, o->sprint};
    for (size_t i = 0; i < kKeys.size(); ++i) {
        if (want[i] == gKeysHeld[i]) continue;
        if (want[i] && vstate::keyDown(kKeys[i].vk)) continue;
        postMoveKey(kKeys[i], want[i]);
        gKeysHeld[i] = want[i];
    }
}

void keyTimer(std::stop_token st) {
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard lock(gKeyMutex);
        if (std::ranges::any_of(gKeysHeld, [](bool b) { return b; }) && !live()) applyKeysLocked(nullptr);
    }
}

json disabled() {
    return {
        {"ok",    false                                         },
        {"code",  "disabled"                                    },
        {"error", "input is off (set \"gameHooks\": true in config.json)"}
    };
}

json badArgs(std::string const& what) {
    return {
        {"ok",    false     },
        {"code",  "bad_args"},
        {"error", what      }
    };
}

} // namespace

void installMoveInputHooks() {
    if (!options().gameHooks || gInstalled) return;
    gInstalled = true;
    int rawRc = 0, stateRc = 0;
    {
        HookPause pause;
        rawRc   = ExtractRawHidHook::hook(false);
        stateRc = UpdatePlayerStateHook::hook(false);
        // Both or neither: one alone would fight the keys of the fallback.
        if (rawRc != 0 || stateRc != 0) {
            if (rawRc == 0) ExtractRawHidHook::unhook(false);
            if (stateRc == 0) UpdatePlayerStateHook::unhook(false);
        } else {
            gRawHooked   = true;
            gStateHooked = true;
        }
    }
    if (viaHooks()) {
        log().info("input: ClientInputUpdateSystem hooks installed");
    } else {
        log().error("input: hooking ClientInputUpdateSystem failed (raw={} state={}); falling back to keys", rawRc, stateRc);
        gKeyTimer = std::jthread(keyTimer);
    }
}

void uninstallMoveInputHooks() {
    if (!gInstalled) return;
    gInstalled = false;
    clearMoveInput();
    if (gKeyTimer.joinable()) {
        gKeyTimer.request_stop();
        gKeyTimer.join();
    }
    HookPause pause;
    if (gRawHooked.exchange(false)) ExtractRawHidHook::unhook(false);
    if (gStateHooked.exchange(false)) UpdatePlayerStateHook::unhook(false);
}

void clearMoveInput() {
    {
        std::lock_guard lock(gMutex);
        gOverride.active = false;
    }
    if (gInstalled && !viaHooks()) {
        std::lock_guard lock(gKeyMutex);
        applyKeysLocked(nullptr);
    }
}

void clearMoveInputOf(uint64_t client) {
    {
        std::lock_guard lock(gMutex);
        if (!gOverride.active || gOverride.owner != client) return;
    }
    if (options().trace) log().info("input: control pipe client {} left: its move override is cleared", client);
    clearMoveInput();
}

json moveInputOp(json const& j) {
    if (!options().gameHooks || !gInstalled) return disabled();
    json r{
        {"ok",  true                       },
        {"via", viaHooks() ? "hooks" : "keys"},
    };
    bool action = j.contains("move") || j.contains("jump") || j.contains("sneak") || j.contains("sprint") || j.contains("ttlMs");
    if (!action) {
        r["active"] = live().has_value();
        r["hooks"]  = {
            {"raw",   gRawHooked.load()  },
            {"state", gStateHooked.load()}
        };
        r["tick"]    = gTicks.load();
        r["applied"] = gApplied.load();
        return r;
    }
    Override o;
    try {
        if (j.contains("move")) {
            auto const& m = j.at("move");
            if (!m.is_object()) return badArgs("move must be {x, z}");
            o.x = std::clamp(m.value("x", 0.0f), -1.0f, 1.0f);
            o.z = std::clamp(m.value("z", 0.0f), -1.0f, 1.0f);
        }
        o.jump   = j.value("jump", false);
        o.sneak  = j.value("sneak", false);
        o.sprint = j.value("sprint", false);
        auto ttl = j.value("ttlMs", int64_t{300});
        if (ttl < 0) return badArgs("ttlMs must be >= 0");
        ttl = std::min(ttl, kMaxTtlMs);
        o.active   = ttl > 0;
        o.deadline = Clock::now() + std::chrono::milliseconds(ttl);
        o.owner    = currentPipeClient();
    } catch (json::exception const& e) {
        return badArgs(e.what());
    }
    if (viaHooks()) {
        std::lock_guard lock(gMutex);
        gOverride = o;
    } else {
        std::lock_guard keys(gKeyMutex);
        {
            std::lock_guard lock(gMutex);
            gOverride = o;
        }
        auto n = normalized(o);
        applyKeysLocked(o.active ? &n : nullptr);
    }
    r["tick"] = gTicks.load();
    return r;
}

} // namespace bedd
