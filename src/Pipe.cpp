// Control pipe: \\.\pipe\BeDD, one JSON object per line in each direction.
// See README.md for the protocol.
#include "BeDD.h"

#include <algorithm>
#include <cctype>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

constexpr int kProtocol = 1;

std::wstring gPipePath; // \\.\pipe\<options().pipeName>

std::jthread      gServer;
std::atomic<bool> gStopping{false};

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

json windowInfo() {
    HWND h = gameWindow();
    json j{
        {"hwnd",       reinterpret_cast<uintptr_t>(h)},
        {"background", backgroundMode().load()     },
        {"managed",    managed()                   },
        {"takenOver",  takenOver()                 },
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
        r["protocol"] = kProtocol;
        r["pid"]      = GetCurrentProcessId();
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
        };
    } else if (op == "background") {
        bool on = j.value("on", true);
        backgroundMode() = on;
        syncAudioMute();
        if (!on) vstate::releaseAll();
        log().info("background mode {} (control pipe)", on ? "on" : "off");
        r.update(windowInfo());
    } else if (op == "window") {
        r["result"] = applyWindowLayout(j.value("clientWidth", 0), j.value("clientHeight", 0), j.value("placement", "none"));
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
    } else if (op == "input") {
        return moveInputOp(j);
    } else {
        throw OpError("unknown op '" + op + "'");
    }
    return r;
}

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

void serveClient(HANDLE pipe) {
    std::string buf;
    char        chunk[4096];
    while (!gStopping) {
        DWORD n = 0;
        if (!ReadFile(pipe, chunk, sizeof(chunk), &n, nullptr) || n == 0) return;
        buf.append(chunk, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string out = handleControlLine(line);
            DWORD       written = 0;
            if (!WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &written, nullptr)) return;
        }
    }
}

void serve(std::stop_token) {
    while (!gStopping) {
        HANDLE pipe = CreateNamedPipeW(
            gPipePath.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            1 << 16,
            1 << 16,
            0,
            nullptr
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            log().error("CreateNamedPipe failed ({}); retrying", GetLastError());
            std::this_thread::sleep_for(1s);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && !gStopping) serveClient(pipe);
        if (connected) clearMoveInput(); // a controller that went away leaves no player running
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

} // namespace

void startPipeServer() {
    std::string const& name = options().pipeName;
    gPipePath               = L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end());
    gStopping               = false;
    gServer                 = std::jthread(serve);
    log().info("control pipe: \\\\.\\pipe\\{}", name);
}

void stopPipeServer() {
    if (!gServer.joinable()) return;
    gStopping = true;
    // Unblock ConnectNamedPipe with a throwaway connection.
    HANDLE h = CreateFileW(gPipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    // ...and a client still connected is blocked in ReadFile.
    CancelSynchronousIo(gServer.native_handle());
    gServer.join();
}

} // namespace bedd
