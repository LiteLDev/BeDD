#include "BeDD.h"
#include "UiTree.h"

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

Options gOptions;

void readOptions(std::filesystem::path const& file) {
    std::ifstream in(file);
    if (!in) {
        log().warn("No {}: using the defaults", file.string());
        return;
    }
    auto j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        log().error("{} is not a JSON object; using defaults", file.string());
        return;
    }
    gOptions.clientWidth  = j.value("clientWidth", gOptions.clientWidth);
    gOptions.clientHeight = j.value("clientHeight", gOptions.clientHeight);
    gOptions.trace        = j.value("trace", gOptions.trace);
    gOptions.wsPort       = j.value("wsPort", gOptions.wsPort);
    if (gOptions.wsPort < 0 || gOptions.wsPort > 65535) {
        log().warn("wsPort: {} is not a port (0..65535); using 0 (an ephemeral port)", gOptions.wsPort);
        gOptions.wsPort = 0;
    }
    gOptions.wsMaxClients = j.value("wsMaxClients", gOptions.wsMaxClients);
    if (auto it = j.find("allowOrigins"); it != j.end() && it->is_array()) {
        for (auto const& o : *it)
            if (o.is_string() && !o.get<std::string>().empty()) gOptions.allowOrigins.push_back(o.get<std::string>());
            else log().warn("allowOrigins: {} is not an origin; ignored", o.dump());
    }
    gOptions.manager      = j.value("manager", gOptions.manager);
    for (char const* gone : {"pipeName", "pipeInstances", "managerScript"})
        if (j.contains(gone)) log().warn("{} is gone (BeDD talks WebSocket now); ignored", gone);
    gOptions.loopbackOnly = j.value("loopbackOnly", gOptions.loopbackOnly);
    if (auto it = j.find("allowHosts"); it != j.end() && it->is_array()) {
        for (auto const& h : *it)
            if (h.is_string() && !h.get<std::string>().empty()) gOptions.allowHosts.push_back(h.get<std::string>());
            else log().warn("allowHosts: {} is not a host name or address; ignored", h.dump());
    }
    gOptions.mode = j.value("mode", gOptions.mode);
    if (gOptions.mode != "ClientOnly" && gOptions.mode != "ClientServer" && gOptions.mode != "Remote") {
        log().warn("mode: '{}' is not ClientOnly, ClientServer or Remote; using ClientOnly", gOptions.mode);
        gOptions.mode = "ClientOnly";
    }
    // A BeDD game is headless: nobody can hear it on purpose, so it is always silent.
    if (j.value("mute", true) == false) log().warn("mute: false is ignored: a BeDD game is always silent");
    gOptions.gameHooks    = j.value("gameHooks", gOptions.gameHooks);
    gOptions.tap          = j.value("tap", gOptions.tap);
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
    if (auto it = j.find("timeScale"); it != j.end()) {
        if (it->is_number() && it->get<double>() >= 0.05 && it->get<double>() <= 20) gOptions.timeScale = it->get<float>();
        else log().warn("timeScale: {} is not a number in 0.05..20; using 1", it->dump());
    }
    gOptions.ui         = j.value("ui", gOptions.ui);
    if (auto it = j.find("uiMaxNodes"); it != j.end()) {
        if (it->is_number_integer() && it->get<int64_t>() >= 1 && it->get<int64_t>() <= 100000) gOptions.uiMaxNodes = it->get<int>();
        else log().warn("uiMaxNodes: {} is not a number in 1..100000; using 1000", it->dump());
    }
}

#ifndef LL_PLAT_S

bool Mod::load() {
    // Flush every message so the harness sees them immediately.
    getSelf().getLogger().setFlushLevel(ll::io::LogLevel::Trace);
    readOptions(getSelf().getConfigDir() / "config.json");
    // Before the game sets up its network (load() runs before enable()).
    if (gOptions.loopbackOnly) installNetHooks();
    // Also before the game window exists: it never shows on the taskbar or the screen.
    startManagerLink();
    installApiHooks();
    // Before the game sets up its audio: not a sound, not even while it loads.
    installAudioHooks();
    log().info(
        "BeDD loaded: client={}x{} trace={} wsPort={} manager={}",
        gOptions.clientWidth,
        gOptions.clientHeight,
        gOptions.trace,
        gOptions.wsPort,
        gOptions.manager
    );
    return true;
}

bool Mod::enable() {
    installApiHooks();
    installGameInputHooks();
    startWindowWatcher();
    installMoveInputHooks(); // before the server: an early "input" must not take the key fallback
    // Before the server: the ops that wait for a tick read its cadence.
    if (gOptions.gameHooks || gOptions.allowSend || gOptions.allowStack) startTickCadence();
    startTap(); // before the server: a client may subscribe as soon as it can connect
    startControlServer();
    startInputTrace();
    startGameState();
    startLook();
    if (gOptions.tapLoopback) installTapLoopback();
    if (gOptions.tap) installTapJournal();
    installGameActions();
    installJoin();
    if (gOptions.allowSend) installSend();
    if (gOptions.allowStack) installStack();
    installTimeScale();
    startUi();
    log().info("BeDD enabled");
    return true;
}

bool Mod::disable() {
    // First: wakes a client thread waiting for a look, so stopControlServer can join it.
    stopLook();
    stopUi(); // first: fails a ui read the pipe thread waits on, so stopPipeServer can join it
    stopGameState();
    uninstallGameActions(); // first: its tick listener drives the virtual mouse
    uninstallJoin(); // first: answers a join the client thread waits on, so stopControlServer() can join it
    uninstallSend(); // first: answers a send the client thread waits on, so stopControlServer() can join it
    uninstallStack(); // likewise for a waiting stack request
    uninstallTimeScale(); // its set waits on the game threads with a timeout (it cannot hang the join)
    stopInputTrace();
    stopManagerLink();
    stopControlServer(); // also deletes the discovery file
    stopWindowWatcher();
    uninstallGameInputHooks();
    uninstallApiHooks();
    uninstallNetHooks();
    uninstallAudioHooks();
    stopTap();
    uninstallTapLoopback();
    uninstallTapJournal();
    uninstallMoveInputHooks();
    stopTickCadence();
    return true;
}

#else // LL_PLAT_S

// In a Bedrock Dedicated Server there is no window, input, audio or manager window to take over:
// the loopback-only network, the WebSocket server and the server units only (xmake.lua builds no
// other source file).
bool Mod::load() {
    getSelf().getLogger().setFlushLevel(ll::io::LogLevel::Trace);
    readOptions(getSelf().getConfigDir() / "config.json");
    // Before the server opens its ports (load() runs before it starts): no firewall prompts.
    if (gOptions.loopbackOnly) installNetHooks();
    log().info(
        "BeDD loaded (server): trace={} wsPort={} loopbackOnly={} timeScale={}",
        gOptions.trace,
        gOptions.wsPort,
        gOptions.loopbackOnly,
        gOptions.timeScale
    );
    return true;
}

bool Mod::enable() {
    startServerTimeScale(); // before the server: the tick counter runs before the first `server` op
    startControlServer();
    log().info("BeDD enabled (server)");
    return true;
}

bool Mod::disable() {
    stopControlServer(); // first: an op waiting on the server thread times out, then its client thread is joined
    stopServerTimeScale();
    uninstallNetHooks();
    return true;
}

#endif // LL_PLAT_S

} // namespace

ll::io::Logger& log() { return Mod::getInstance().getSelf().getLogger(); }

Options& options() { return gOptions; }

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
