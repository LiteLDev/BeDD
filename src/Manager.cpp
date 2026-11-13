// The manager: BeDD.exe's window connects to every game's WebSocket server and sends
// `{op:"manager", action:"attach"}`; it shows the game (DWM thumbnail) and closes it when it is
// closed. The game window always stays off screen, off the taskbar and out of Alt+Tab (Window.cpp,
// ApiHooks.cpp), attached or not: the manager is the only place that shows it.
//
// With config manager (default true) a game never runs without the manager: if none attached within
// 30 s of the game window existing, or once the attached connection dropped and no manager attached
// again within 3 s (BeDD.exe restarting its connection), the game closes. With
// manager:false it stays (hidden) whatever happens, e.g. for a harness that drives it directly.
// `{op:"close"}` closes the game.
#include "BeDD.h"
#include "Discovery.h"

#include <format>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

// How long a game waits for its first attach once its window exists (BeDD.exe connects as soon as
// the discovery file appears, and the window comes a little later).
constexpr auto kAttachDeadline = 30s;
// How long a manager whose connection dropped has to attach again.
constexpr auto kReattachGrace = 3s;

std::jthread gWatcher;

using Clock = std::chrono::steady_clock;
std::mutex        gMutex;
uint64_t          gAttached = 0;   // the client that attached (0 = none), guarded by gMutex
bool              gEverAttached = false; // guarded by gMutex
Clock::time_point gLostAt{};       // the attached connection dropped (0 = no), guarded by gMutex

// A game the manager does not show must not run at all.
[[noreturn]] void closeUnmanaged(char const* why) {
    log().error("{}: closing the game (config manager: a BeDD game only runs inside the manager window)", why);
    deleteDiscovery(); // disable() does not run
    TerminateProcess(GetCurrentProcess(), 1);
    ExitProcess(1);
}

// The attached manager's connection dropped (on its departing client thread; not at shutdown).
void onGone(uint64_t client) {
    {
        std::lock_guard lock(gMutex);
        if (client == 0 || client != gAttached) return;
        gAttached = 0;
    }
    setManaged(false);
    if (options().manager) {
        log().warn("the manager (client {}) went away; the game closes unless it attaches again within 3 s", client);
        std::lock_guard lock(gMutex);
        gLostAt = Clock::now();
        return;
    }
    log().info("the manager (client {}) went away; the game stays (config manager is false)", client);
}

// Config manager: closes the game when no manager attached within kAttachDeadline of its window
// existing, or none attached again within kReattachGrace of the attached one going away.
void watch(std::stop_token stop) {
    auto windowSince = Clock::time_point{};
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(100ms);
        auto now = Clock::now();
        bool ever = false;
        auto lostAt = Clock::time_point{};
        {
            std::lock_guard lock(gMutex);
            ever   = gEverAttached;
            lostAt = gLostAt;
        }
        if (stop.stop_requested()) break;
        if (lostAt != Clock::time_point{} && now - lostAt >= kReattachGrace) closeUnmanaged("the manager went away");
        if (ever || !gameWindow()) continue;
        if (windowSince == Clock::time_point{}) windowSince = now;
        if (now - windowSince >= kAttachDeadline) closeUnmanaged("no manager attached within 30 s of the game window");
    }
}

} // namespace

void startManagerLink() {
    static bool registered = false;
    if (!registered) {
        registered = true;
        onClientGone(onGone);
    }
    if (options().manager && !gWatcher.joinable()) gWatcher = std::jthread(watch);
}

void stopManagerLink() {
    if (!gWatcher.joinable()) return;
    gWatcher.request_stop();
    gWatcher.join();
}

json managerOp(json const& req) {
    std::string action = req.value("action", "");
    uint64_t    client = currentClient();
    if (action == "attach") {
        if (!client) return {{"ok", false}, {"code", "bad_request"}, {"error", "attach needs a client connection"}};
        uint64_t before = 0;
        {
            std::lock_guard lock(gMutex);
            before        = gAttached;
            gAttached     = client;
            gEverAttached = true;
            gLostAt       = {};
        }
        setManaged(true);
        if (before != client) log().info("the manager attached (client {}{})", client, before ? std::format(", replacing client {}", before) : "");
    } else if (!action.empty()) {
        return {{"ok", false}, {"code", "bad_request"}, {"error", "unknown manager action '" + action + "' (attach)"}};
    }
    std::lock_guard lock(gMutex);
    return {
        {"ok",       true                                   },
        {"attached", gAttached != 0                         },
        {"you",      gAttached != 0 && gAttached == client  },
        {"manager",  options().manager                      },
        {"pid",      GetCurrentProcessId()                  },
        {"hwnd",     reinterpret_cast<uintptr_t>(gameWindow())},
        {"instance", instanceName()                         },
        {"label",    label()                                },
    };
}

json closeOp(json const&) {
    HWND h = gameWindow();
    if (!h || !PostMessageW(h, WM_CLOSE, 0, 0))
        return {{"ok", false}, {"code", "not_ready"}, {"error", "no game window to close"}};
    log().info("client {} asked the game to close", currentClient());
    return {{"ok", true}};
}

} // namespace bedd
