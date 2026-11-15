// FMOD hooks: the game is silent unless the user turns its sound on in the manager window (a config
// "mute": false is ignored: a headless game is heard by nobody on purpose; only the attached manager
// connection can make one audible, `{op:"manager", action:"audio"}` -> setAudible(), and it goes
// silent again when that connection drops). The game hears no focus loss, so it never suspends its
// own audio, and every headless game would otherwise play its menu music and sounds on the user's
// speakers.
//
// Silent = the master channel group of every FMOD system is muted, whatever the game's own setMute()
// calls on it say (it unmutes it when it believes it regained the focus). The game keeps the audio
// output it chose: FMOD resolves the output once, in System::init, and a running system ignores
// System::setOutput (checked with getOutput: it stays NOSOUND), so a game started on the NOSOUND
// output could never be heard. So each game has a session in the Windows volume mixer, a silent one.
//
// Audible = each master group gets back the mute state the game last asked for. The switch happens
// in the system's next System::update (every frame, on the thread that drives that system, so also
// for a system initialised with FMOD_INIT_THREAD_UNSAFE); without that hook, a thread-safe system
// is switched right away and a thread-unsafe one at its next playSound.
#include "BeDD.h"

#include <algorithm>
#include <array>
#include <atomic>
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
using UpdateFn    = int (*)(void* system);

constexpr unsigned kInitThreadUnsafe = 0x00100000; // FMOD_INIT_THREAD_UNSAFE

constexpr char kInit[] = "?init@System@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAX@Z";
constexpr char kPlaySound[] =
    "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z";
constexpr char kMaster[]  = "?getMasterChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z";
constexpr char kSetMute[] = "?setMute@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z";
constexpr char kUpdate[]  = "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ";

InitFn      o_init      = nullptr;
PlaySoundFn o_playSound = nullptr;
SetMuteFn   o_setMute   = nullptr;
UpdateFn    o_update    = nullptr;
MasterFn    gGetMaster  = nullptr;

bool              gInstalled = false;
std::atomic<bool> gAudible{false}; // the manager turned the sound on

struct System {
    void* system;
    void* master;
    bool  gameMute = false; // the master mute the game last asked for
    bool  unsafe   = false; // FMOD_INIT_THREAD_UNSAFE: switched on its own thread only
    bool  pending  = false; // a switch waits for its next update (or playSound)
    bool  applied  = false; // audible as of its last switch
};
// Never held while calling FMOD: FMOD takes its own lock, and its threads may call back in.
std::mutex                              gMutex;
std::vector<System>                     gSystems;   // guarded by gMutex
std::vector<std::pair<void*, unsigned>> gInitFlags; // init flags until tracked, guarded by gMutex

System* findLocked(void* system) {
    for (auto& s : gSystems)
        if (s.system == system) return &s;
    return nullptr;
}

// Remember the system's master group and mute it (unless audible).
void track(void* system) {
    {
        std::lock_guard lock(gMutex);
        if (findLocked(system)) return;
    }
    void* master = nullptr;
    if (!o_setMute || gGetMaster(system, &master) != 0 || !master) return;
    bool audible = gAudible.load();
    bool unsafe  = false;
    {
        std::lock_guard lock(gMutex);
        if (findLocked(system)) return;
        System s{system, master};
        for (auto it = gInitFlags.begin(); it != gInitFlags.end(); ++it) {
            if (it->first != system) continue;
            s.unsafe = (it->second & kInitThreadUnsafe) != 0;
            gInitFlags.erase(it);
            break;
        }
        s.applied = audible;
        unsafe    = s.unsafe;
        gSystems.push_back(s);
    }
    log().info(
        "FMOD system {}: {}{}",
        system,
        audible ? "audible" : "muted",
        unsafe ? " (initialised thread-unsafe)" : ""
    );
    o_setMute(master, !audible);
}

// A switch the system still waits for, done on its own thread (its update or playSound).
void applyPending(void* system) {
    void* master   = nullptr;
    bool  gameMute = false;
    bool  audible  = gAudible.load();
    {
        std::lock_guard lock(gMutex);
        System*         s = findLocked(system);
        if (!s || !s->pending) return;
        s->pending = false;
        if (s->applied == audible) return;
        s->applied = audible;
        master     = s->master;
        gameMute   = s->gameMute;
    }
    o_setMute(master, gameMute || !audible);
}

int d_init(void* system, int maxChannels, unsigned flags, void* extra) {
    {
        std::lock_guard lock(gMutex);
        std::erase_if(gInitFlags, [&](auto const& f) { return f.first == system; });
        gInitFlags.emplace_back(system, flags);
    }
    int rc = o_init(system, maxChannels, flags, extra);
    if (rc == 0) track(system);
    return rc;
}

int d_update(void* system) {
    applyPending(system);
    return o_update(system);
}

int d_playSound(void* system, void* sound, void* group, bool paused, void** channel) {
    track(system); // a system initialised before the hooks
    applyPending(system);
    return o_playSound(system, sound, group, paused, channel);
}

// While silent, the game unmuting a master group (it does when it regains focus) keeps it muted.
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
    return o_setMute(control, mute || (master && !gAudible.load()));
}

struct FmodHook {
    char const* name;
    void*       detour;
    void**      original;
    void*       target = nullptr;
};
std::array<FmodHook, 4> gHooks{
    FmodHook{kSetMute,   reinterpret_cast<void*>(&d_setMute),   reinterpret_cast<void**>(&o_setMute)  },
    FmodHook{kInit,      reinterpret_cast<void*>(&d_init),      reinterpret_cast<void**>(&o_init)     },
    FmodHook{kPlaySound, reinterpret_cast<void*>(&d_playSound), reinterpret_cast<void**>(&o_playSound)},
    FmodHook{kUpdate,    reinterpret_cast<void*>(&d_update),    reinterpret_cast<void**>(&o_update)   },
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
        log().info("FMOD hooks installed: the game is silent until its sound is turned on in the manager window");
    else log().error("FMOD hooks installed: {}/{}; the game's audio may not be muted", installed, gHooks.size());
}

void uninstallAudioHooks() {
    if (!gInstalled) return;
    gInstalled = false;
    // The game stays as it is: it is shutting down, and an unmuted master would let its last sounds out.
    HookPause pause;
    for (auto& h : gHooks) {
        if (!h.target) continue;
        ll::memory::unhook(h.target, h.detour, false);
        h.target    = nullptr;
        *h.original = nullptr;
    }
}

bool setAudible(bool on) {
    if (!o_setMute) return false;
    bool                before = gAudible.exchange(on);
    std::vector<System> now;
    {
        std::lock_guard lock(gMutex);
        for (auto& s : gSystems) {
            if (o_update || s.unsafe) {
                s.pending = true; // at its next update (or playSound), on its own thread
                continue;
            }
            if (s.applied == on) continue;
            s.applied = on;
            now.push_back(s);
        }
    }
    for (auto const& s : now) o_setMute(s.master, s.gameMute || !on);
    if (before != on) log().info("the manager turned the game's sound {}", on ? "on (it plays on this PC)" : "off");
    return true;
}

bool audible() { return gAudible.load(); }

std::string audioStats() {
    std::lock_guard lock(gMutex);
    if (!o_setMute) return "not hooked";
    return std::to_string(gSystems.size()) + " FMOD system(s), " + (gAudible.load() ? "audible" : "muted");
}

} // namespace bedd
