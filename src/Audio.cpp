// FMOD hooks: in background mode the game is silent. The game hears no focus loss there, so it
// never suspends its own audio, and every headless game would otherwise play its menu music and
// sounds on the user's speakers. The master channel group of each FMOD system is muted while
// background mode is on (a human taking the game over hears it again); the game's own
// setMute() calls on it are remembered and win once background mode ends.
#include "BeDD.h"

#include <array>
#include <mutex>
#include <vector>

#include "ll/api/memory/Hook.h"

namespace bedd {

namespace {

// FMOD::System and FMOD::ChannelControl members, called with the object as the first argument.
using InitFn      = int (*)(void* system, int maxChannels, unsigned flags, void* extra);
using PlaySoundFn = int (*)(void* system, void* sound, void* group, bool paused, void** channel);
using MasterFn    = int (*)(void* system, void** group);
using SetMuteFn   = int (*)(void* control, bool mute);

constexpr char kInit[] = "?init@System@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAX@Z";
constexpr char kPlaySound[] =
    "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z";
constexpr char kMaster[]  = "?getMasterChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z";
constexpr char kSetMute[] = "?setMute@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z";

constexpr unsigned kInitThreadUnsafe = 0x00100000; // FMOD_INIT_THREAD_UNSAFE

InitFn      o_init      = nullptr;
PlaySoundFn o_playSound = nullptr;
SetMuteFn   o_setMute   = nullptr;
MasterFn    gGetMaster  = nullptr;

bool gInstalled = false;

struct System {
    void* system;
    void* master;
    bool  gameMute; // what the game itself last asked for
};
// Never held while calling FMOD: FMOD takes its own lock, and its threads may call back in.
std::mutex          gMutex;
std::vector<System> gSystems; // guarded by gMutex

bool muteNow() { return options().mute && backgroundMode().load(std::memory_order_relaxed); }

// Remember the system's master group and mute it as it should be now.
void track(void* system) {
    {
        std::lock_guard lock(gMutex);
        for (auto const& s : gSystems)
            if (s.system == system) return;
    }
    void* master = nullptr;
    if (!o_setMute || gGetMaster(system, &master) != 0 || !master) return;
    {
        std::lock_guard lock(gMutex);
        for (auto const& s : gSystems)
            if (s.system == system) return;
        gSystems.push_back({system, master, false});
    }
    o_setMute(master, muteNow());
}

int d_init(void* system, int maxChannels, unsigned flags, void* extra) {
    // The mute changes from other threads (control pipe, manager), so FMOD must lock its API.
    if (options().mute) flags &= ~kInitThreadUnsafe;
    int rc = o_init(system, maxChannels, flags, extra);
    if (rc == 0) track(system);
    return rc;
}

int d_playSound(void* system, void* sound, void* group, bool paused, void** channel) {
    track(system); // a system initialised before the hooks
    return o_playSound(system, sound, group, paused, channel);
}

int d_setMute(void* control, bool mute) {
    bool master = false;
    {
        std::lock_guard lock(gMutex);
        for (auto& s : gSystems) {
            if (s.master != control) continue;
            s.gameMute = mute;
            master     = true;
        }
    }
    return o_setMute(control, mute || (master && muteNow()));
}

struct FmodHook {
    char const* name;
    void*       detour;
    void**      original;
    void*       target = nullptr;
};
std::array<FmodHook, 3> gHooks{
    FmodHook{kSetMute,   reinterpret_cast<void*>(&d_setMute),   reinterpret_cast<void**>(&o_setMute)  },
    FmodHook{kInit,      reinterpret_cast<void*>(&d_init),      reinterpret_cast<void**>(&o_init)     },
    FmodHook{kPlaySound, reinterpret_cast<void*>(&d_playSound), reinterpret_cast<void**>(&o_playSound)},
};

} // namespace

void installAudioHooks() {
    if (gInstalled) return;
    gInstalled   = true;
    HMODULE fmod = GetModuleHandleW(L"fmod.dll");
    if (!fmod) {
        log().warn("fmod.dll is not loaded; the game's audio is not muted");
        return;
    }
    gGetMaster = reinterpret_cast<MasterFn>(GetProcAddress(fmod, kMaster));
    // Resolved before the pause: GetProcAddress takes loader locks a paused thread may hold.
    for (auto& h : gHooks) h.target = reinterpret_cast<void*>(GetProcAddress(fmod, h.name));
    int installed = 0;
    {
        HookPause pause;
        for (auto& h : gHooks) {
            if (!gGetMaster || !h.target) continue;
            int rc = ll::memory::hook(
                h.target,
                h.detour,
                reinterpret_cast<ll::memory::FuncPtr*>(h.original),
                ll::memory::HookPriority::Highest,
                false
            );
            if (rc != 0 || !*h.original) {
                *h.original = nullptr;
                h.target    = nullptr;
                continue;
            }
            ++installed;
        }
    }
    if (installed == static_cast<int>(gHooks.size()))
        log().info("FMOD hooks installed: the game is silent in background mode");
    else log().error("FMOD hooks installed: {}/{}; the game's audio may not be muted", installed, gHooks.size());
}

void uninstallAudioHooks() {
    if (!gInstalled) return;
    gInstalled = false;
    // The game stays muted: it is shutting down, and an unmuted master would let its last sounds out.
    HookPause pause;
    for (auto& h : gHooks) {
        if (!h.target) continue;
        ll::memory::unhook(h.target, h.detour, false);
        h.target    = nullptr;
        *h.original = nullptr;
    }
}

void syncAudioMute() {
    if (!o_setMute) return;
    std::vector<System> systems;
    {
        std::lock_guard lock(gMutex);
        systems = gSystems;
    }
    bool mute = muteNow();
    for (auto const& s : systems) o_setMute(s.master, s.gameMute || mute);
}

std::string audioStats() {
    std::lock_guard lock(gMutex);
    if (!o_setMute) return "not hooked";
    return std::to_string(gSystems.size()) + " FMOD system(s), " + (muteNow() ? "muted" : "audible");
}

} // namespace bedd
