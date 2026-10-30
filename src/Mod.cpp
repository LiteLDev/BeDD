#include "BeDD.h"

#include <array>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"

namespace bedd {

namespace {

class Mod {
public:
    static Mod& getInstance() {
        static Mod instance;
        return instance;
    }

    Mod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};

Options           gOptions;
std::atomic<bool> gBackground{false};

void readOptions(std::filesystem::path const& file) {
    std::ifstream in(file);
    if (!in) {
        log().info("No {} (background mode off; the e2e harness writes it)", file.string());
        return;
    }
    auto j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        log().error("{} is not a JSON object; using defaults", file.string());
        return;
    }
    gOptions.background   = j.value("background", gOptions.background);
    gOptions.placement    = j.value("placement", gOptions.placement);
    gOptions.clientWidth  = j.value("clientWidth", gOptions.clientWidth);
    gOptions.clientHeight = j.value("clientHeight", gOptions.clientHeight);
    gOptions.trace        = j.value("trace", gOptions.trace);
    gOptions.pipeName     = j.value("pipeName", gOptions.pipeName);
    gOptions.loopbackOnly = j.value("loopbackOnly", gOptions.loopbackOnly);
    gOptions.manager      = j.value("manager", gOptions.manager);
    gOptions.mute         = j.value("mute", gOptions.mute);
    gOptions.gameHooks    = j.value("gameHooks", gOptions.gameHooks);
    gOptions.tap          = j.value("tap", gOptions.tap);
    gOptions.pipeInstances = j.value("pipeInstances", gOptions.pipeInstances);
    gOptions.tapLoopback  = gOptions.tap && j.value("tapLoopback", gOptions.tapLoopback);
    gOptions.tapJournalMiB = j.value("tapJournalMiB", gOptions.tapJournalMiB);
    gOptions.allowSend    = j.value("allowSend", gOptions.allowSend);
    if (auto it = j.find("sendAllow"); it != j.end() && it->is_array()) {
        gOptions.sendAllow.clear();
        for (auto const& id : *it)
            if (id.is_number_integer() && id.get<int64_t>() >= 0 && id.get<int64_t>() <= 0x3ff) gOptions.sendAllow.push_back(id.get<int>());
            else log().warn("sendAllow: {} is not a packet id (0..1023); ignored", id.dump());
    }
    gOptions.allowStack   = j.value("allowStack", gOptions.allowStack);
}

bool Mod::load() {
    // Flush every message so the harness sees them immediately.
    getSelf().getLogger().setFlushLevel(ll::io::LogLevel::Trace);
    readOptions(getSelf().getConfigDir() / "config.json");
    gBackground = gOptions.background;
    // Before the game sets up its network (load() runs before enable()).
    if (gOptions.background && gOptions.loopbackOnly) installNetHooks();
    // Also before the game window exists: a managed game's window never shows on the taskbar.
    startManagerLink();
    if (gOptions.background) installApiHooks();
    // Before the game sets up its audio: not a sound, not even while it loads.
    if (gOptions.mute) installAudioHooks();
    log().info(
        "BeDD loaded: background={} placement={} client={}x{} trace={} pipe={} manager={} mute={}",
        gOptions.background,
        gOptions.placement,
        gOptions.clientWidth,
        gOptions.clientHeight,
        gOptions.trace,
        gOptions.pipeName,
        gOptions.manager,
        gOptions.mute
    );
    return true;
}

bool Mod::enable() {
    installApiHooks();
    installGameInputHooks();
    startWindowWatcher();
    installMoveInputHooks(); // before the pipe: an early "input" must not take the key fallback
    startPipeServer();
    startInputTrace();
    startGameState();
    startLook();
    startTap();
    if (gOptions.tapLoopback) installTapLoopback();
    if (gOptions.tap) installTapJournal();
    installGameActions();
    if (gOptions.allowSend) installSend();
    if (gOptions.allowStack) installStack();
    log().info("BeDD enabled");
    return true;
}

bool Mod::disable() {
    // First: wakes a pipe thread waiting for a look, so stopPipeServer can join it.
    stopLook();
    stopGameState();
    uninstallGameActions(); // first: its tick listener drives the virtual mouse
    uninstallSend(); // first: answers a send the pipe thread waits on, so stopPipeServer() can join it
    stopInputTrace();
    stopManagerLink();
    stopPipeServer();
    stopWindowWatcher();
    uninstallGameInputHooks();
    uninstallApiHooks();
    uninstallNetHooks();
    uninstallAudioHooks();
    stopTap();
    uninstallTapLoopback();
    uninstallTapJournal();
    uninstallMoveInputHooks();
    uninstallStack();
    return true;
}

} // namespace

ll::io::Logger& log() { return Mod::getInstance().getSelf().getLogger(); }

Options& options() { return gOptions; }

std::atomic<bool>& backgroundMode() { return gBackground; }

// ---------------------------------------------------------------------------------------------
// Virtual input state
// ---------------------------------------------------------------------------------------------
namespace vstate {
namespace {
std::array<std::atomic<uint8_t>, 256> gKeys{}; // bit 0x80 = down, 0x01 = toggled
std::mutex                            gCursorMutex;
POINT                                 gCursor{};

void setOne(int vk, bool down) {
    if (vk <= 0 || vk > 255) return;
    auto& k   = gKeys[static_cast<size_t>(vk)];
    uint8_t v = k.load();
    if (down && !(v & 0x80)) v ^= 0x01; // a new press flips the toggle (Caps Lock style)
    v = down ? static_cast<uint8_t>(v | 0x80) : static_cast<uint8_t>(v & ~0x80);
    k.store(v);
}

// The generic modifier is down while either side is.
void syncGeneric(int generic, int left, int right) { setOne(generic, keyDown(left) || keyDown(right)); }
} // namespace

void setKey(int vk, bool down) {
    switch (vk) {
    case VK_SHIFT: setOne(VK_LSHIFT, down); break;
    case VK_CONTROL: setOne(VK_LCONTROL, down); break;
    case VK_MENU: setOne(VK_LMENU, down); break;
    default: setOne(vk, down); break;
    }
    syncGeneric(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);
    syncGeneric(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
    syncGeneric(VK_MENU, VK_LMENU, VK_RMENU);
}

bool keyDown(int vk) { return vk > 0 && vk <= 255 && (gKeys[static_cast<size_t>(vk)].load() & 0x80); }

SHORT keyState(int vk) {
    if (vk <= 0 || vk > 255) return 0;
    uint8_t v = gKeys[static_cast<size_t>(vk)].load();
    return static_cast<SHORT>(((v & 0x80) ? 0x8000 : 0) | (v & 0x01));
}

void setCursor(POINT client) {
    std::lock_guard lock(gCursorMutex);
    gCursor = client;
}

POINT cursor() {
    std::lock_guard lock(gCursorMutex);
    return gCursor;
}

void releaseAll() {
    for (auto& k : gKeys) k.store(static_cast<uint8_t>(k.load() & 0x01));
}
} // namespace vstate

} // namespace bedd

LL_REGISTER_MOD(bedd::Mod, bedd::Mod::getInstance());
