// The control protocol: one JSON request per WebSocket text message, one JSON reply per request,
// in order on each connection (WsServer.cpp is the transport). See docs/reference/protocol.md.
// handle() dispatches the ops; the rest of this file ties connections to their clients (held keys,
// the disconnect cleanup) and starts the WebSocket server and its discovery file.
#include "BeDD.h"
#include "Discovery.h"
#include "Screenshot.h"
#include "WsServer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

constexpr int kProtocol = 2;

// The client (WebSocket connection) the current thread serves (0 = none: the game thread).
thread_local uint64_t                        tClient = 0;
thread_local std::shared_ptr<ws::Connection> tConnection;
// Run on the connection's thread once the reply of the current request is sent (afterReply()).
thread_local std::vector<std::function<void()>> tAfterReply;

#ifndef LL_PLAT_S // the game client's ops; a server's are below (#else)

// Which client last pressed each virtual key / mouse button (0 = nobody, or not a client).
std::array<std::atomic<uint64_t>, 256> gKeyOwner{};

// The generic modifiers are their left keys in the virtual state (vstate::setKey): one owner slot each.
int ownerSlot(int vk) {
    switch (vk) {
    case VK_SHIFT: return VK_LSHIFT;
    case VK_CONTROL: return VK_LCONTROL;
    case VK_MENU: return VK_LMENU;
    default: return vk;
    }
}

void setOwner(int vk, bool down) {
    vk = ownerSlot(vk);
    if (vk > 0 && vk < 256) gKeyOwner[static_cast<size_t>(vk)] = down ? tClient : 0;
}

struct OpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

int vkFromName(std::string name) {
    for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static std::unordered_map<std::string, int> const names{
        {"ESC",       VK_ESCAPE },
        {"ESCAPE",    VK_ESCAPE },
        {"ENTER",     VK_RETURN },
        {"RETURN",    VK_RETURN },
        {"BACK",      VK_BACK   },
        {"BACKSPACE", VK_BACK   },
        {"TAB",       VK_TAB    },
        {"SPACE",     VK_SPACE  },
        {"SHIFT",     VK_SHIFT  },
        {"LSHIFT",    VK_LSHIFT },
        {"RSHIFT",    VK_RSHIFT },
        {"CTRL",      VK_CONTROL},
        {"CONTROL",   VK_CONTROL},
        {"LCTRL",     VK_LCONTROL},
        {"RCTRL",     VK_RCONTROL},
        {"ALT",       VK_MENU   },
        {"LEFT",      VK_LEFT   },
        {"UP",        VK_UP     },
        {"RIGHT",     VK_RIGHT  },
        {"DOWN",      VK_DOWN   },
        {"HOME",      VK_HOME   },
        {"END",       VK_END    },
        {"PGUP",      VK_PRIOR  },
        {"PGDN",      VK_NEXT   },
        {"INSERT",    VK_INSERT },
        {"DELETE",    VK_DELETE },
        {"DEL",       VK_DELETE },
        {"SLASH",     VK_OEM_2  },
        {"/",         VK_OEM_2  },
        {"GRAVE",     VK_OEM_3  },
        {"MINUS",     VK_OEM_MINUS},
        {"EQUALS",    VK_OEM_PLUS},
    };
    if (auto it = names.find(name); it != names.end()) return it->second;
    if (name.size() >= 2 && name[0] == 'F' && std::isdigit(static_cast<unsigned char>(name[1]))) {
        int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (name.size() == 1 && (std::isalnum(static_cast<unsigned char>(name[0])))) return name[0];
    throw OpError("unknown key name '" + name + "'");
}

int vkOf(json const& j) {
    if (j.contains("vk")) return j["vk"].get<int>();
    if (j.contains("key")) return vkFromName(j["key"].get<std::string>());
    throw OpError("key op needs 'key' or 'vk'");
}

bool isExtended(int vk) {
    switch (vk) {
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_INSERT:
    case VK_DELETE:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_DIVIDE: return true;
    default: return false;
    }
}

// The virtual key a WM_KEYDOWN carries for a left/right-specific key.
int messageVk(int vk) {
    switch (vk) {
    case VK_LSHIFT:
    case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL:
    case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU:
    case VK_RMENU: return VK_MENU;
    default: return vk;
    }
}

void postKey(int vk, bool down) {
    UINT   sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (static_cast<LPARAM>(sc & 0xFF) << 16) | (isExtended(vk) ? (1 << 24) : 0);
    if (!down) lp |= (1LL << 30) | (1LL << 31);
    else if (vstate::keyDown(vk)) lp |= (1LL << 30); // auto-repeat
    bool sys = vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_F10;
    UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    if (!post(Event{.msg = msg, .wp = static_cast<WPARAM>(messageVk(vk)), .lp = lp, .vk = vk, .vkDown = down}))
        throw OpError("no game window yet");
    setOwner(vk, down);
}

WPARAM mouseKeyFlags() {
    WPARAM f = 0;
    if (vstate::keyDown(VK_LBUTTON)) f |= MK_LBUTTON;
    if (vstate::keyDown(VK_RBUTTON)) f |= MK_RBUTTON;
    if (vstate::keyDown(VK_MBUTTON)) f |= MK_MBUTTON;
    if (vstate::keyDown(VK_SHIFT)) f |= MK_SHIFT;
    if (vstate::keyDown(VK_CONTROL)) f |= MK_CONTROL;
    return f;
}

POINT clientSize() {
    RECT r{};
    GetClientRect(gameWindow(), &r);
    return {r.right, r.bottom};
}

// Target point of a mouse op: x/y in client pixels or rx/ry as fractions of the client area;
// the current virtual cursor when neither is given.
POINT targetPoint(json const& j) {
    POINT p = vstate::cursor();
    if (j.contains("rx") && j.contains("ry")) {
        POINT s = clientSize();
        p       = {static_cast<LONG>(j["rx"].get<double>() * s.x), static_cast<LONG>(j["ry"].get<double>() * s.y)};
    } else if (j.contains("x") && j.contains("y")) {
        p = {j["x"].get<LONG>(), j["y"].get<LONG>()};
    }
    return p;
}

LPARAM clientLParam(POINT p) { return MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y)); }

void postMove(POINT p) {
    // The pointer is already over the window, so WM_MOUSEMOVE alone (no enter/hover messages).
    if (!post(Event{.msg = WM_MOUSEMOVE, .wp = mouseKeyFlags(), .lp = clientLParam(p), .moveCursor = true, .cursor = p}))
        throw OpError("no game window yet");
}

// Mouse buttons and the wheel: Bedrock reads them only through GameInput (WM_LBUTTONDOWN and
// friends are ignored), so they drive the virtual GameInput mouse. The pointer position comes from
// GetCursorPos (hooked: the virtual cursor) and WM_MOUSEMOVE.
struct Button {
    int vk;
    int gi;
};

Button buttonOf(json const& j) {
    std::string b = j.value("button", "left");
    if (b == "left") return {VK_LBUTTON, kGiLeft};
    if (b == "right") return {VK_RBUTTON, kGiRight};
    if (b == "middle") return {VK_MBUTTON, kGiMiddle};
    throw OpError("unknown button '" + b + "'");
}

void setButton(Button const& b, bool down) {
    vstate::setKey(b.vk, down);
    gameInputMouse(0, 0, 0, b.gi, down);
    setOwner(b.vk, down);
}

void sleepMs(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// GameInput is polled once per frame: keep each state long enough for the game to read it.
constexpr int kPollMs = 50;

void flushOrThrow(json const& j) {
    auto timeout = std::chrono::milliseconds(j.value("timeoutMs", 5000));
    if (!flush(timeout)) throw OpError("the game did not process the input within the timeout (game thread busy?)");
}

// The keys and mouse buttons down in the virtual state, and the client that pressed each (`state`).
json heldInput() {
    json held = json::array();
    for (int vk = 1; vk < 256; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue; // their left keys are listed
        if (!vstate::keyDown(vk)) continue;
        held.push_back({
            {"vk",     vk                                         },
            {"client", gKeyOwner[static_cast<size_t>(vk)].load()},
        });
    }
    return held;
}

json windowInfo() {
    HWND h = gameWindow();
    json j{
        {"hwnd",       reinterpret_cast<uintptr_t>(h)},
        {"managed",    managed()                   },
    };
    if (h) {
        POINT s     = clientSize();
        j["client"] = {{"w", s.x}, {"h", s.y}};
        POINT c     = vstate::cursor();
        j["cursor"] = {{"x", c.x}, {"y", c.y}};
        j["realForeground"] = real::GetForegroundWindow() == h;
    }
    return j;
}

json handle(json const& j) {
    std::string op = j.value("op", "");
    json        r{
        {"ok", true}
    };
    if (op == "ping") {
        r["protocol"]    = kProtocol;
        r["pid"]         = GetCurrentProcessId();
        r["kind"]        = "client";
        r["defaultMode"] = "bedboo";
        r["modes"]       = {
            {"bedboo", {"world", "look", "input", "slot", "action", "button", "send", "stack", "timescale", "join", "tap"}},
            {"input",  {"key", "text", "mouse", "raw", "release", "flush", "focus", "window", "screenshot"}              },
            {"ui",     {"ui"}                                                                                      },
        };
        r.update(windowInfo());
    } else if (op == "state") {
        auto& s = stats();
        r.update(windowInfo());
        r["stats"] = {
            {"synthetic",          s.synthetic.load()         },
            {"swallowedInput",     s.swallowedInput.load()    },
            {"swallowedFocus",     s.swallowedFocus.load()    },
            {"passed",             s.passed.load()            },
            {"realForegroundSeen", s.realForegroundSeen.load()},
            {"api",                apiHookStats()             },
            {"gameInput",          gameInputStats()           },
            {"loopbackRebinds",    loopbackRebinds()          },
            {"serverQueriesRefused", serverQueriesRefused()     },
            {"audio",              audioStats()               },
            {"tapLoopback",        [] {
                 auto t = tapLoopbackStats();
                 return json{{"enabled", t.enabled}, {"hooks", t.hooks}, {"link", t.link}, {"clientbound", t.clientbound}, {"echo", t.echo},
                             {"bytes", t.bytes}, {"unpublished", t.unpublished}, {"guest", t.guest}};
             }()},
            {"send",               sendStats()                },
        };
        json tap = json::parse(tapHooks());
        for (auto& [name, on] : tap.items()) r["hooks"][name] = on;
        r["clients"] = connectedClients();
        r["held"]    = heldInput();
        r["mode"]       = options().mode;
        r["connection"] = connectionInfo();
        r["stats"]["sendsRefused"] = sendsRefused();
        auto jn               = tapJournalStats();
        r["stats"]["journal"] = {
            {"enabled",   jn.enabled  },
            {"frames",    jn.frames   },
            {"bytes",     jn.bytes    },
            {"complete",  jn.complete },
            {"seen",      jn.seen     },
            {"compacted", jn.compacted},
            {"pruned",    jn.pruned   },
            {"transient", jn.transient},
            {"overCap",   jn.overCap  },
            {"resets",    jn.resets   },
            {"replays",   jn.replays  },
        };
    } else if (op == "window") {
        r["result"] = applyWindowLayout(j.value("clientWidth", 0), j.value("clientHeight", 0));
        r.update(windowInfo());
    } else if (op == "focus") {
        fakeFocusGained();
        flushOrThrow(j);
    } else if (op == "key") {
        int         vk     = vkOf(j);
        std::string action = j.value("action", "press");
        if (action == "down" || action == "press") postKey(vk, true);
        if (action == "press") {
            flushOrThrow(j);
            sleepMs(j.value("holdMs", 60));
        }
        if (action == "up" || action == "press") postKey(vk, false);
        if (action != "down" && action != "up" && action != "press") throw OpError("unknown key action " + action);
        flushOrThrow(j);
    } else if (op == "text") {
        std::string  text = j.at("text").get<std::string>();
        int          n    = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), w.data(), n);
        int delay = j.value("delayMs", 0);
        for (wchar_t c : w) {
            if (!post(Event{.msg = WM_CHAR, .wp = static_cast<WPARAM>(c), .lp = 1})) throw OpError("no game window yet");
            if (delay > 0) {
                flushOrThrow(j);
                sleepMs(delay);
            }
        }
        flushOrThrow(j);
    } else if (op == "mouse" || op == "raw") {
        // mouse: absolute pointer (menus, inventories). raw: relative movement (the in-world
        // camera while the mouse is grabbed). Buttons and wheel work the same in both.
        bool        raw    = op == "raw";
        std::string action = j.value("action", "move");
        if (!raw && (j.contains("x") || j.contains("rx") || action == "move")) {
            postMove(targetPoint(j));
            flushOrThrow(j);
            if (action != "move") sleepMs(j.value("settleMs", kPollMs)); // hover first, like a hand would
        }
        if (raw) {
            int64_t dx = j.value("dx", 0), dy = j.value("dy", 0);
            // Spread a move over a few frames (one huge delta can be clamped by the game).
            int steps = std::max(1, j.value("steps", 1));
            for (int i = 0; i < steps; ++i) {
                gameInputMouse(dx * (i + 1) / steps - dx * i / steps, dy * (i + 1) / steps - dy * i / steps, 0, 0, false);
                if (i + 1 < steps) sleepMs(16);
            }
        }
        if (action == "down" || action == "up") {
            setButton(buttonOf(j), action == "down");
        } else if (action == "click") {
            Button b = buttonOf(j);
            setButton(b, true);
            sleepMs(std::max(j.value("holdMs", 80), kPollMs));
            setButton(b, false);
        } else if (action == "wheel") {
            gameInputMouse(0, 0, j.value("notches", 1), 0, false);
        } else if (action != "move") {
            throw OpError("unknown " + op + " action " + action);
        }
        sleepMs(j.value("afterMs", kPollMs));
    } else if (op == "release") {
        clearMoveInput();
        for (int vk = 1; vk < 256; ++vk) {
            if (!vstate::keyDown(vk)) continue;
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
                json b{{"button", vk == VK_LBUTTON ? "left" : vk == VK_RBUTTON ? "right" : "middle"}};
                setButton(buttonOf(b), false);
            } else if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) {
                postKey(vk, false);
            }
        }
        flushOrThrow(j);
    } else if (op == "flush") {
        flushOrThrow(j);
    } else if (op == "world") {
        r = worldOp(j);
    } else if (op == "look") {
        r = lookOp(j);
    } else if (op == "tap") {
        return tapOp(j);
    } else if (op == "slot") {
        r = gameActionOp(op, j);
    } else if (op == "action") {
        r = gameActionOp(op, j);
    } else if (op == "button") {
        r = gameActionOp(op, j);
    } else if (op == "input") {
        return moveInputOp(j);
    } else if (op == "send") {
        r = sendOp(j);
    } else if (op == "stack") {
        r = stackOp(j);
    } else if (op == "timescale") {
        r = timeScaleOp(j);
    } else if (op == "join") {
        r = joinOp(j);
    } else if (op == "manager") {
        r = managerOp(j);
    } else if (op == "close") {
        r = closeOp(j);
    } else if (op == "screenshot") {
        r = screenshotOp(j);
    } else {
        throw OpError("unknown op '" + op + "'");
    }
    return r;
}

#else // LL_PLAT_S

// Inside a Bedrock Dedicated Server: no window and no input, so the server's own ops only.
json handle(json const& j) {
    std::string op = j.value("op", "");
    json        r{
        {"ok", true}
    };
    if (op == "ping") {
        r["protocol"] = kProtocol;
        r["pid"]      = GetCurrentProcessId();
        r["kind"]     = "server";
        r["target"]   = "server";
    } else if (op == "state") {
        r["target"]       = "server";
        r["clients"]      = connectedClients();
        r["loopbackOnly"] = options().loopbackOnly;
        r["stats"]        = {
            {"loopbackRebinds",      loopbackRebinds()     },
            {"serverQueriesRefused", serverQueriesRefused()},
        };
    } else if (op == "server") {
        r = serverInfoOp(j);
    } else if (op == "timescale") {
        r = serverTimeScaleOp(j);
    } else if (op == "command") {
        r = serverCommandOp(j);
    } else {
        throw std::runtime_error("unknown op '" + op + "' (this is the server target)");
    }
    return r;
}

#endif // LL_PLAT_S

} // namespace

std::string handleControlLine(std::string const& line) {
    json req = json::parse(line, nullptr, false);
    json resp;
    if (req.is_discarded() || !req.is_object()) {
        resp = {
            {"ok",    false             },
            {"error", "not a JSON object"}
        };
    } else {
        try {
            resp = handle(req);
        } catch (std::exception const& e) {
            resp = {
                {"ok",    false   },
                {"error", e.what()}
            };
        }
        if (req.contains("id")) resp["id"] = req["id"];
    }
    return resp.dump() + "\n";
}

namespace {

// ---------------------------------------------------------------------------------------------
// Clients: one per WebSocket connection (up to options().wsMaxClients), each served by its own
// thread (WsServer.cpp), which handles its requests in order.
// ---------------------------------------------------------------------------------------------
std::mutex                                 gGoneMutex;
std::vector<std::function<void(uint64_t)>> gGoneCallbacks; // guarded by gGoneMutex

// Serializes the ops that post input, so two clients cannot interleave a press or a typed text.
std::mutex gInputMutex;

// Ops that run without gInputMutex: they post no window input. `tap` and `world` only read;
// `look`, `input`, `send`, `stack`, `timescale` and `join` hand their work to the client thread (and
// may wait for it, up to their timeoutMs), so serializing them would only stall the other clients.
// `screenshot` only reads the window, and may wait for the game thread to answer PrintWindow.
bool concurrentOp(std::string const& line) {
#ifdef LL_PLAT_S
    (void)line;
    return true; // a server posts no window input (and its ops may wait on the server thread)
#endif
    json req = json::parse(line, nullptr, false);
    if (!req.is_object()) return true;
    std::string op = req.value("op", "");
    return op == "ping" || op == "state" || op == "tap" || op == "world" || op == "look" || op == "input" || op == "send"
        || op == "stack" || op == "timescale" || op == "join" || op == "manager" || op == "close"
        || op == "screenshot";
}

std::string handleClientLine(std::string const& line) {
    if (concurrentOp(line)) return handleControlLine(line);
    std::lock_guard lock(gInputMutex);
    return handleControlLine(line);
}

#ifndef LL_PLAT_S // a server holds no input for its clients

// Lets go of the keys and mouse buttons the client still holds (and nobody pressed after it).
// A key-down is in the virtual state only once the game thread dispatched it, so a key it owns is
// released even when it does not look down yet (a stray key-up is harmless).
void releaseClientInput(uint64_t id) {
    bool posted = false;
    for (int vk = 1; vk < 256; ++vk) {
        if (gKeyOwner[static_cast<size_t>(vk)] != id) continue;
        bool button = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON;
        if (button && !vstate::keyDown(vk)) continue;
        try {
            if (button) {
                setButton(Button{vk, vk == VK_LBUTTON ? kGiLeft : vk == VK_RBUTTON ? kGiRight : kGiMiddle}, false);
            } else {
                postKey(vk, false);
                posted = true;
            }
            log().info("client {} left holding vk {:#x}: released", id, vk);
        } catch (std::exception const& e) {
            log().warn("client {}: could not release vk {:#x}: {}", id, vk, e.what());
        }
    }
    if (posted) flush(1s);
}

#endif // LL_PLAT_S

// The per-client disconnect path: everything a client left behind is released here. Units with
// per-client state register with onClientGone() (or add their one-line release call below).
void clientGone(uint64_t id) {
    std::lock_guard input(gInputMutex);
#ifndef LL_PLAT_S
    releaseClientInput(id);
    clearMoveInputOf(id);      // the `input` override, if this client set it (MoveInput.cpp)
    releaseGameButtonsOf(id);  // `button` holds of this client (GameActions.cpp)
#endif
    std::vector<std::function<void(uint64_t)>> callbacks;
    {
        std::lock_guard lock(gGoneMutex);
        callbacks = gGoneCallbacks;
    }
    for (auto& fn : callbacks) {
        try {
            fn(id);
        } catch (std::exception const& e) {
            log().warn("client {}: disconnect callback failed: {}", id, e.what());
        }
    }
}

// The WebSocket handlers, all on the connection's own thread.
void onOpen(std::shared_ptr<ws::Connection> const& conn) {
    tClient     = conn->id();
    tConnection = conn;
}

void onMessage(std::shared_ptr<ws::Connection> const& conn, uint8_t opcode, std::string&& data) {
    std::string out;
    if (opcode != ws::kText) {
        out = json{
            {"ok",    false                                       },
            {"code",  "bad_request"                               },
            {"error", "requests are JSON objects in text messages"},
        }.dump();
    } else {
        out = handleClientLine(data);
        if (!out.empty() && out.back() == '\n') out.pop_back();
    }
    conn->sendText(out);
    auto after = std::move(tAfterReply);
    tAfterReply.clear();
    for (auto& fn : after) {
        try {
            fn();
        } catch (std::exception const& e) {
            log().warn("client {}: {}", conn->id(), e.what());
        }
    }
}

void onClosed(std::shared_ptr<ws::Connection> const& conn, bool stopping) {
    // At shutdown the game thread is the one stopping us: posting (and flushing) input would only time out.
    if (!stopping) clientGone(conn->id());
    tConnection.reset();
    tClient = 0;
}

} // namespace

uint64_t currentClient() { return tClient; }

std::shared_ptr<ws::Connection> currentConnection() { return tConnection; }

void afterReply(std::function<void()> fn) { tAfterReply.push_back(std::move(fn)); }

int connectedClients() { return ws::connections(); }

void onClientGone(std::function<void(uint64_t)> fn) {
    std::lock_guard lock(gGoneMutex);
    gGoneCallbacks.push_back(std::move(fn));
}

void startControlServer() {
    std::string token;
    try {
        token = ws::newToken();
    } catch (std::exception const& e) {
        log().error("websocket: no token ({}); not listening", e.what());
        return;
    }
    int const  maxClients = std::max(1, options().wsMaxClients);
    ws::Config config{
        .port         = options().wsPort,
        .maxClients   = maxClients,
        .token        = token,
        .allowOrigins = options().allowOrigins,
    };
    int port = ws::start(std::move(config), {.open = onOpen, .message = onMessage, .closed = onClosed});
    if (!port) return;
    log().info("websocket: ws://127.0.0.1:{}/v1 ({} clients at once)", port, maxClients);
    writeDiscovery(port, token);
}

void stopControlServer() {
    deleteDiscovery(); // first: nobody should find a server that is going away
    ws::stop();
}

} // namespace bedd
