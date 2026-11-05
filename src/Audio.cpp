// FMOD hooks: the game is always silent (a config "mute": false is ignored: a headless game is
// heard by nobody on purpose). The game hears no focus loss, so it never suspends its own audio,
// and every headless game would otherwise play its menu music and sounds on the user's speakers.
// Each FMOD system the game initialises gets the NOSOUND output: it mixes in real time as before,
// but opens no audio device, so the game has no session in the Windows volume mixer either. The
// game picks an output itself (System::setOutput, also after init, which switches the output of a
// running system): every such call gets NOSOUND too, so no later switch (an audio device change,
// say) opens a device again. The master channel group of each system also stays muted (a system
// initialised before the hooks keeps its device), whatever the game's own setMute() calls on it say.
#include "BeDD.h"

#include <algorithm>
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
using SetOutputFn = int (*)(void* system, int output);

constexpr int kOutputNoSound = 2; // FMOD_OUTPUTTYPE_NOSOUND (NOSOUND_NRT would mix as fast as it can)

constexpr char kInit[] = "?init@System@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAX@Z";
constexpr char kPlaySound[] =
    "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z";
constexpr char kMaster[]  = "?getMasterChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z";
constexpr char kSetMute[] = "?setMute@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z";
constexpr char kSetOutput[] = "?setOutput@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z";

InitFn      o_init      = nullptr;
PlaySoundFn o_playSound = nullptr;
SetMuteFn   o_setMute   = nullptr;
SetOutputFn o_setOutput = nullptr;
MasterFn    gGetMaster  = nullptr;
SetOutputFn gSetOutput  = nullptr; // the export (through d_setOutput once hooked)

bool gInstalled = false;

struct System {
    void* system;
    void* master;
};
// Never held while calling FMOD: FMOD takes its own lock, and its threads may call back in.
std::mutex          gMutex;
std::vector<System> gSystems; // guarded by gMutex

// Remember the system's master group and mute it.
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
        gSystems.push_back({system, master});
    }
    o_setMute(master, true);
}

int d_setOutput(void* system, int output) {
    if (output != kOutputNoSound) {
        log().info("FMOD system {}: the game asked for output {}; NOSOUND instead", system, output);
        output = kOutputNoSound;
    }
    return o_setOutput(system, output);
}

int d_init(void* system, int maxChannels, unsigned flags, void* extra) {
    // Before init: the game's own choice of output (made before the hooks, or never) is overridden.
    if (gSetOutput) {
        int out = gSetOutput(system, kOutputNoSound);
        if (out == 0) log().info("FMOD system {}: NOSOUND output (no audio device)", system);
        else log().warn("FMOD system {}: setOutput(NOSOUND) failed ({}); only muted", system, out);
    }
    int rc = o_init(system, maxChannels, flags, extra);
    if (rc == 0) track(system);
    return rc;
}

int d_playSound(void* system, void* sound, void* group, bool paused, void** channel) {
    track(system); // a system initialised before the hooks
    return o_playSound(system, sound, group, paused, channel);
}

// The game unmuting a master group (it does when it regains focus) keeps it muted.
int d_setMute(void* control, bool mute) {
    bool master = false;
    {
        std::lock_guard lock(gMutex);
        master = std::ranges::any_of(gSystems, [&](System const& s) { return s.master == control; });
    }
    return o_setMute(control, mute || master);
}

struct FmodHook {
    char const* name;
    void*       detour;
    void**      original;
    void*       target = nullptr;
};
std::array<FmodHook, 4> gHooks{
    FmodHook{kSetMute,   reinterpret_cast<void*>(&d_setMute),   reinterpret_cast<void**>(&o_setMute)  },
    FmodHook{kSetOutput, reinterpret_cast<void*>(&d_setOutput), reinterpret_cast<void**>(&o_setOutput)},
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
    gSetOutput = reinterpret_cast<SetOutputFn>(GetProcAddress(fmod, kSetOutput));
    if (!gSetOutput) log().warn("FMOD has no System::setOutput; the game is muted but keeps its audio device");
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
        log().info("FMOD hooks installed: the game is silent");
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

std::string audioStats() {
    std::lock_guard lock(gMutex);
    if (!o_setMute) return "not hooked";
    return std::to_string(gSystems.size()) + " FMOD system(s), muted";
}

} // namespace bedd
