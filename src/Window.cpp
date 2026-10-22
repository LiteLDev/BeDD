// The game window: subclassing, synthetic input dispatch, focus shield and placement.
#include "Headless.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace headless {

namespace {

std::atomic<HWND>  gWindow{nullptr};
std::atomic<DWORD> gGameThread{0};
WNDPROC            gOrigProc = nullptr;
UINT               gInputMsg = 0;

std::mutex              gQueueMutex;
std::deque<Event>       gQueue;
uint64_t                gPosted = 0; // guarded by gQueueMutex
uint64_t                gDispatched = 0;
std::condition_variable gDispatchedCv;

std::mutex                              gRawMutex;
std::unordered_map<uintptr_t, RAWMOUSE> gRaw;
uintptr_t                               gNextRaw = 0x7E5D'0000'0000ull;

thread_local bool tSynthetic = false;

Stats gStats;
// Per-message counters for trace mode (index = message id, 0x400 = anything at or above WM_USER).
std::array<std::atomic<uint32_t>, 0x401> gMsgCounts{};

std::jthread      gWatcher;
HWND              gReturnFocus = nullptr;

bool bg() { return backgroundMode().load(std::memory_order_relaxed); }

bool isClientInput(UINT m) {
    return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_MOUSEHOVER
        || m == WM_MOUSELEAVE || (m >= 0x0241 && m <= 0x0257) /* WM_POINTER* */ || m == WM_TOUCH
        || m == WM_INPUT;
}

void dispatchOne(HWND h) {
    Event e;
    {
        std::lock_guard lock(gQueueMutex);
        if (gQueue.empty()) return;
        e = gQueue.front();
        gQueue.pop_front();
    }
    if (e.vk >= 0) vstate::setKey(e.vk, e.vkDown);
    if (e.moveCursor) vstate::setCursor(e.cursor);
    LPARAM lp = e.lp;
    uintptr_t rawHandle = 0;
    if (e.raw) {
        std::lock_guard lock(gRawMutex);
        rawHandle       = gNextRaw++;
        gRaw[rawHandle] = e.rawMouse;
        lp              = static_cast<LPARAM>(rawHandle);
    }
    if (e.msg) {
        tSynthetic = true;
        gStats.synthetic.fetch_add(1, std::memory_order_relaxed);
        CallWindowProcW(gOrigProc, h, e.msg, e.wp, lp);
        tSynthetic = false;
    }
    if (rawHandle) {
        std::lock_guard lock(gRawMutex);
        gRaw.erase(rawHandle);
    }
    {
        std::lock_guard lock(gQueueMutex);
        ++gDispatched;
    }
    gDispatchedCv.notify_all();
}

LRESULT CALLBACK subclassProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == gInputMsg) {
        dispatchOne(h);
        return 0;
    }
    if (options().trace) gMsgCounts[m < 0x400 ? m : 0x400].fetch_add(1, std::memory_order_relaxed);
    if (!tSynthetic && bg()) {
        switch (m) {
        case WM_ACTIVATEAPP:
            if (!w) {
                gStats.swallowedFocus.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) {
                gStats.swallowedFocus.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
        case WM_CAPTURECHANGED: gStats.swallowedFocus.fetch_add(1, std::memory_order_relaxed); return 0;
        case WM_NCACTIVATE:
            // Let Windows draw the inactive caption, but don't tell the game.
            if (!w) return DefWindowProcW(h, m, w, l);
            break;
        default: break;
        }
        if (isClientInput(m)) {
            gStats.swallowedInput.fetch_add(1, std::memory_order_relaxed);
            // Human takeover: Ctrl+Shift+F12 on the real keyboard ends background mode.
            if (m == WM_KEYDOWN && w == VK_F12 && (real::GetAsyncKeyState(VK_CONTROL) & 0x8000)
                && (real::GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                backgroundMode() = false;
                vstate::releaseAll();
                log().warn("Ctrl+Shift+F12: background mode off, real input goes to the game again");
            }
            // Real raw input still needs DefWindowProc for the system's cleanup.
            if (m == WM_INPUT) return DefWindowProcW(h, m, w, l);
            return 0;
        }
    }
    gStats.passed.fetch_add(1, std::memory_order_relaxed);
    return CallWindowProcW(gOrigProc, h, m, w, l);
}

DWORD processOf(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

std::wstring classOf(HWND h) {
    wchar_t buf[256]{};
    GetClassNameW(h, buf, 256);
    return buf;
}

std::string narrow(std::wstring const& w) {
    if (w.empty()) return {};
    int         n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The largest visible, unowned top-level window of this process.
HWND findGameWindow() {
    struct Ctx {
        DWORD pid;
        HWND  best = nullptr;
        long  area = 0;
    } ctx{GetCurrentProcessId()};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(p);
            if (processOf(h) != c.pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            RECT r{};
            GetWindowRect(h, &r);
            long area = (r.right - r.left) * (r.bottom - r.top);
            if (area > c.area) {
                c.area = area;
                c.best = h;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx)
    );
    return ctx.best;
}

bool isGoodFocusTarget(HWND h) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) return false;
    if (processOf(h) == GetCurrentProcessId()) return false;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    return true;
}

// Hand the foreground back (to the window that had it when the game started, else the next
// window below in Z order). Allowed because this process is the foreground process.
void giveAwayForeground(HWND game) {
    if (real::GetForegroundWindow() != game) return;
    HWND target = isGoodFocusTarget(gReturnFocus) ? gReturnFocus : nullptr;
    for (HWND w = GetTopWindow(nullptr); !target && w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w != game && isGoodFocusTarget(w) && !GetWindow(w, GW_OWNER)) target = w;
    }
    if (!target) return;
    real::SetForegroundWindow(target);
    log().info("gave the foreground back to {} [{}]", static_cast<void*>(target), narrow(classOf(target)));
}

std::string processNameOf(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH]{};
    DWORD   n = MAX_PATH;
    QueryFullProcessImageNameW(p, 0, buf, &n);
    CloseHandle(p);
    std::wstring s(buf, n);
    auto         slash = s.find_last_of(L"\\/");
    return narrow(slash == std::wstring::npos ? s : s.substr(slash + 1));
}

void rememberReturnFocus() {
    if (char const* env = std::getenv("LEVI_HEADLESS_RETURN_FOCUS")) {
        gReturnFocus = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(env, nullptr, 0)));
        return;
    }
    HWND fg = real::GetForegroundWindow();
    if (!fg || processOf(fg) == GetCurrentProcessId()) return;
    std::string name = processNameOf(processOf(fg));
    if (_stricmp(name.c_str(), "LeviLauncher.exe") == 0) return; // the launcher closes soon
    gReturnFocus = fg;
}

void traceTick() {
    std::string msgs;
    for (size_t i = 0; i < gMsgCounts.size(); ++i) {
        uint32_t n = gMsgCounts[i].exchange(0);
        if (!n) continue;
        msgs += std::format(" 0x{:04X}={}", i, n);
    }
    log().info(
        "trace: synthetic={} swallowedInput={} swallowedFocus={} passed={} realForeground={} | msgs:{} | api: {}",
        gStats.synthetic.load(),
        gStats.swallowedInput.load(),
        gStats.swallowedFocus.load(),
        gStats.passed.load(),
        real::GetForegroundWindow() == gWindow.load(),
        msgs.empty() ? " none" : msgs,
        apiHookStats()
    );
}

void subclass(HWND h) {
    gGameThread = GetWindowThreadProcessId(h, nullptr);
    gOrigProc   = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&subclassProc))
    );
    gWindow = h;
    RECT c{};
    GetClientRect(h, &c);
    log().info(
        "game window {} [{}] subclassed (client {}x{}, thread {})",
        static_cast<void*>(h),
        narrow(classOf(h)),
        c.right,
        c.bottom,
        gGameThread.load()
    );
}

void watch(std::stop_token stop) {
    using namespace std::chrono;
    auto lastTrace  = steady_clock::now();
    auto guardUntil = steady_clock::time_point{};
    while (!stop.stop_requested()) {
        HWND h = gWindow.load();
        if (!h || !IsWindow(h)) {
            if (h) {
                log().warn("game window {} is gone", static_cast<void*>(h));
                gWindow = nullptr;
            }
            if (HWND found = findGameWindow()) {
                subclass(found);
                auto& o = options();
                if (bg()) {
                    log().info("{}", applyWindowLayout(o.clientWidth, o.clientHeight, o.placement));
                    fakeFocusGained();
                    // The game may still activate itself while it starts up: keep handing the
                    // foreground back for a while (later activations are a human clicking it).
                    guardUntil = steady_clock::now() + seconds(30);
                } else if (o.clientWidth > 0 && o.clientHeight > 0) {
                    log().info("{}", applyWindowLayout(o.clientWidth, o.clientHeight, "none"));
                }
            }
        } else if (bg()) {
            if (real::GetForegroundWindow() == h) {
                gStats.realForegroundSeen.fetch_add(1, std::memory_order_relaxed);
                if (steady_clock::now() < guardUntil) giveAwayForeground(h);
            }
        }
        if (options().trace && steady_clock::now() - lastTrace > seconds(5)) {
            lastTrace = steady_clock::now();
            traceTick();
        }
        std::this_thread::sleep_for(milliseconds(100));
    }
}

} // namespace

HWND gameWindow() { return gWindow.load(std::memory_order_relaxed); }

bool gameThreadIsCurrent() { return GetCurrentThreadId() == gGameThread.load(std::memory_order_relaxed); }

Stats& stats() { return gStats; }

bool post(Event const& e) {
    HWND h = gWindow.load();
    if (!h) return false;
    {
        std::lock_guard lock(gQueueMutex);
        gQueue.push_back(e);
        ++gPosted;
    }
    if (!PostMessageW(h, gInputMsg, 0, 0)) {
        std::lock_guard lock(gQueueMutex);
        gQueue.pop_back();
        --gPosted;
        return false;
    }
    return true;
}

bool flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock(gQueueMutex);
    uint64_t target = gPosted;
    return gDispatchedCv.wait_for(lock, timeout, [&] { return gDispatched >= target; });
}

RAWMOUSE const* syntheticRawMouse(HRAWINPUT h) {
    std::lock_guard lock(gRawMutex);
    auto            it = gRaw.find(reinterpret_cast<uintptr_t>(h));
    return it == gRaw.end() ? nullptr : &it->second;
}

std::string applyWindowLayout(int clientW, int clientH, std::string const& placement) {
    HWND h = gWindow.load();
    if (!h) return "no game window yet";
    std::string done;
    int         x = 40, y = 40;
    if (clientW > 0 && clientH > 0) {
        RECT r{0, 0, clientW, clientH};
        AdjustWindowRectExForDpi(
            &r,
            static_cast<DWORD>(GetWindowLongW(h, GWL_STYLE)),
            FALSE,
            static_cast<DWORD>(GetWindowLongW(h, GWL_EXSTYLE)),
            GetDpiForWindow(h)
        );
        int w = r.right - r.left, hgt = r.bottom - r.top;
        if (placement == "offscreen") x = GetSystemMetrics(SM_XVIRTUALSCREEN) - w - 64;
        if (IsZoomed(h) || IsIconic(h)) {
            // Restore without activating: SetWindowPlacement with SW_SHOWNOACTIVATE.
            WINDOWPLACEMENT wp{sizeof(WINDOWPLACEMENT)};
            GetWindowPlacement(h, &wp);
            wp.showCmd          = SW_SHOWNOACTIVATE;
            wp.rcNormalPosition = RECT{x, y, x + w, y + hgt};
            SetWindowPlacement(h, &wp);
        }
        SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        RECT c{};
        GetClientRect(h, &c);
        done = std::format("client {}x{} at ({},{})", c.right, c.bottom, x, y);
    } else if (placement == "offscreen") {
        RECT wr{};
        GetWindowRect(h, &wr);
        x = GetSystemMetrics(SM_XVIRTUALSCREEN) - (wr.right - wr.left) - 64;
        SetWindowPos(h, nullptr, x, wr.top, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
        done = std::format("moved off screen to x={}", x);
    }
    if (placement == "back" || placement == "offscreen") {
        SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        giveAwayForeground(h);
        done += done.empty() ? "" : ", ";
        done += "sent to the back of the Z order";
    }
    return "window layout: " + (done.empty() ? std::string("unchanged") : done);
}

void fakeFocusGained() {
    HWND h = gWindow.load();
    if (!h) return;
    post(Event{.msg = WM_ACTIVATEAPP, .wp = TRUE, .lp = 0});
    post(Event{.msg = WM_ACTIVATE, .wp = WA_ACTIVE, .lp = 0});
    post(Event{.msg = WM_SETFOCUS, .wp = 0, .lp = 0});
}

void startWindowWatcher() {
    gInputMsg = RegisterWindowMessageW(L"LeviHeadlessInput.Event");
    rememberReturnFocus();
    gWatcher = std::jthread(watch);
}

void stopWindowWatcher() {
    if (gWatcher.joinable()) {
        gWatcher.request_stop();
        gWatcher.join();
    }
    HWND h = gWindow.exchange(nullptr);
    if (h && gOrigProc && IsWindow(h)) SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gOrigProc));
}

} // namespace headless
