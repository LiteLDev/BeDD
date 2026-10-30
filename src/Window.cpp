// The game window: subclassing, synthetic input dispatch, focus shield and placement.
#include "BeDD.h"

#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace bedd {

namespace {

std::atomic<HWND>  gWindow{nullptr};
std::atomic<DWORD> gGameThread{0};
WNDPROC            gOrigProc = nullptr;
UINT               gInputMsg = 0;
// Window changes the manager asks for run on the game thread (wParam: SyncAction, lParam: ticket):
// from another thread they would wait for the game thread, which may be waiting for that thread.
UINT gSyncMsg = 0;
enum SyncAction : WPARAM { kSync, kTakeOver, kHandBack };
std::mutex              gActionMutex;
std::condition_variable gActionCv;
uint64_t                gActionPosted = 0; // guarded by gActionMutex
uint64_t                gActionDone   = 0;
std::string             gActionResult;

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

// Manager window (Manager.cpp): while linked, the game window is a tool window (no taskbar button,
// not in Alt+Tab) kept off screen; the manager shows it through a DWM thumbnail.
std::atomic<bool> gManaged{false};
std::atomic<bool> gTakenOver{false};
std::atomic<bool> gCreatedHidden{false}; // the game window was created hidden (CreateWindowExW)
// Set on a window this mod made a tool window (findGameWindow skips other tool windows).
wchar_t const* const kHiddenProp = L"BeDD.Hidden";

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

void onSync(HWND h, WPARAM action, LPARAM ticket);

LRESULT CALLBACK subclassProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == gInputMsg) {
        dispatchOne(h);
        return 0;
    }
    if (m == gSyncMsg) {
        onSync(h, w, l);
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
                syncAudioMute();
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
            if ((GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) && !GetPropW(h, kHiddenProp)) return TRUE;
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

// Managed, not taken over and in background mode: the manager shows the game, its window hides.
bool hideNow() { return gManaged.load() && !gTakenOver.load() && bg(); }

// Left of the whole virtual desktop, so no monitor shows it.
int offScreenX(int windowWidth) { return GetSystemMetrics(SM_XVIRTUALSCREEN) - windowWidth - 64; }

bool isOnScreen(HWND h) {
    RECT r{}, v{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)}, both{};
    v.right  = v.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    v.bottom = v.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    GetWindowRect(h, &r);
    return IntersectRect(&both, &r, &v) != FALSE;
}

void setTaskbarButton(HWND h, bool shown) {
    // The taskbar does not follow WS_EX_TOOLWINDOW changes of a visible window: tell it directly.
    HRESULT       init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITaskbarList* list = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))) {
        if (SUCCEEDED(list->HrInit())) shown ? list->AddTab(h) : list->DeleteTab(h);
        list->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
}

// Tool window (no taskbar button, not in Alt+Tab) or back to a normal one.
void setHidden(HWND h, bool hidden) {
    LONG_PTR ex   = GetWindowLongPtrW(h, GWL_EXSTYLE);
    LONG_PTR want = hidden ? (ex | WS_EX_TOOLWINDOW) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW) : ex & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
    if (hidden) SetPropW(h, kHiddenProp, reinterpret_cast<HANDLE>(1));
    if (want == ex) return;
    SetWindowLongPtrW(h, GWL_EXSTYLE, want);
    real::SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (IsWindowVisible(h)) setTaskbarButton(h, !hidden);
    log().info("game window {} {}", static_cast<void*>(h), hidden ? "hidden from the taskbar and Alt+Tab (manager)" : "back on the taskbar");
}

bool isHidden(HWND h) { return GetPropW(h, kHiddenProp) && (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW); }

// Centred in the primary monitor's work area, keeping its size.
void bringOnScreen(HWND h) {
    MONITORINFO mi{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r{};
    GetWindowRect(h, &r);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    int x = mi.rcWork.left + std::max(0L, (mi.rcWork.right - mi.rcWork.left - w) / 2);
    int y = mi.rcWork.top + std::max(0L, (mi.rcWork.bottom - mi.rcWork.top - hgt) / 2);
    real::SetWindowPos(h, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool needsSync(HWND h) { return hideNow() ? !isHidden(h) || isOnScreen(h) : isHidden(h); }

// Brings the window in line with hideNow(): a hidden tool window off screen, or a normal window
// on screen. On the game thread only (requestSync): when the manager link or takeover changes, and
// whenever the watcher sees it out of line (the game, a script or Ctrl+Shift+F12 can change things).
void syncManaged(HWND h) {
    if (hideNow()) {
        bool changed = !isHidden(h);
        if (changed) setHidden(h, true);
        // Again after a style change: the frame (and so the client area) is smaller now.
        if (changed || isOnScreen(h)) log().info("{}", applyWindowLayout(options().clientWidth, options().clientHeight, "offscreen"));
    } else if (isHidden(h)) {
        setHidden(h, false);
        if (!isOnScreen(h)) bringOnScreen(h);
    }
}

void requestSync() {
    HWND h = gWindow.load();
    if (h && gSyncMsg) PostMessageW(h, gSyncMsg, kSync, 0);
}

// Runs a take-over or hand-back on the game thread and waits for it, at most a few seconds (the
// game thread may be busy loading, or waiting for this thread in disable()).
std::string runOnGameThread(HWND h, SyncAction action) {
    std::unique_lock lock(gActionMutex);
    uint64_t         ticket = ++gActionPosted;
    if (!PostMessageW(h, gSyncMsg, action, static_cast<LPARAM>(ticket))) return "the game window is gone";
    if (!gActionCv.wait_for(lock, std::chrono::seconds(5), [&] { return gActionDone >= ticket; }))
        return "requested (the game is busy and does it as soon as it can)";
    return gActionResult;
}

void onSync(HWND h, WPARAM action, LPARAM ticket) {
    std::string result;
    if (action == kTakeOver) {
        syncManaged(h);
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        if (!isOnScreen(h)) bringOnScreen(h);
        real::SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        // The manager allowed this process to take the foreground (AllowSetForegroundWindow).
        result = real::SetForegroundWindow(h) ? "taken over" : "taken over (Windows did not let the window come to the front)";
        log().warn("taken over from the manager window: background mode off, real input goes to the game");
    } else if (action == kHandBack) {
        syncManaged(h);
        fakeFocusGained();
        result = "handed back";
        log().info("handed back to the scripts from the manager window: background mode on");
    } else {
        syncManaged(h);
        return;
    }
    {
        std::lock_guard lock(gActionMutex);
        gActionDone   = std::max(gActionDone, static_cast<uint64_t>(ticket));
        gActionResult = result;
    }
    gActionCv.notify_all();
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
    char buf[32]{};
    if (GetEnvironmentVariableA("BEDD_RETURN_FOCUS", buf, sizeof(buf)) > 0) {
        gReturnFocus = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(buf, nullptr, 0)));
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
        "trace: synthetic={} swallowedInput={} swallowedFocus={} passed={} realForeground={} | msgs:{} | api: {} | gameinput: {}",
        gStats.synthetic.load(),
        gStats.swallowedInput.load(),
        gStats.swallowedFocus.load(),
        gStats.passed.load(),
        real::GetForegroundWindow() == gWindow.load(),
        msgs.empty() ? " none" : msgs,
        apiHookStats(),
        gameInputStats()
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
    auto lastSync   = steady_clock::now();
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
                    requestSync();
                    log().info("{}", applyWindowLayout(o.clientWidth, o.clientHeight, o.placement));
                    fakeFocusGained();
                    // The game may still activate itself while it starts up: keep handing the
                    // foreground back for a while (later activations are a human clicking it).
                    guardUntil = steady_clock::now() + seconds(30);
                } else if (o.clientWidth > 0 && o.clientHeight > 0) {
                    log().info("{}", applyWindowLayout(o.clientWidth, o.clientHeight, "none"));
                }
            }
        } else {
            if (bg() && real::GetForegroundWindow() == h) {
                gStats.realForegroundSeen.fetch_add(1, std::memory_order_relaxed);
                if (steady_clock::now() < guardUntil) giveAwayForeground(h);
            }
            if ((gManaged || GetPropW(h, kHiddenProp)) && steady_clock::now() - lastSync > milliseconds(500)) {
                lastSync = steady_clock::now();
                if (needsSync(h)) requestSync();
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

bool inSyntheticDispatch() { return tSynthetic; }

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

std::string applyWindowLayout(int clientW, int clientH, std::string const& requested) {
    HWND h = gWindow.load();
    if (!h) return "no game window yet";
    // The manager shows the game: wherever a script asks for it, the window stays off screen.
    std::string const& placement = hideNow() && bg() ? std::string("offscreen") : requested;
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
        if (placement == "offscreen") x = offScreenX(w);
        if (IsZoomed(h) || IsIconic(h)) {
            // Restore without activating: SetWindowPlacement with SW_SHOWNOACTIVATE.
            WINDOWPLACEMENT wp{sizeof(WINDOWPLACEMENT)};
            GetWindowPlacement(h, &wp);
            wp.showCmd          = SW_SHOWNOACTIVATE;
            wp.rcNormalPosition = RECT{x, y, x + w, y + hgt};
            SetWindowPlacement(h, &wp);
        }
        real::SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        RECT c{};
        GetClientRect(h, &c);
        done = std::format("client {}x{} at ({},{})", c.right, c.bottom, x, y);
    } else if (placement == "offscreen") {
        RECT wr{};
        GetWindowRect(h, &wr);
        x = offScreenX(wr.right - wr.left);
        real::SetWindowPos(h, nullptr, x, wr.top, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
        done = std::format("moved off screen to x={}", x);
    }
    if (placement == "back" || placement == "offscreen") {
        real::SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

void setManaged(bool on) {
    if (gManaged.exchange(on) == on) return;
    if (!on) gTakenOver = false;
    requestSync();
}

bool managed() { return gManaged.load(); }

bool takenOver() { return gTakenOver.load(); }

void prepareFirstShow(HWND h) {
    if (!hideNow() || IsWindowVisible(h) || GetWindow(h, GW_OWNER) || (GetWindowLongW(h, GWL_STYLE) & WS_CHILD)) return;
    // Before the first show: no taskbar button and no frame on screen, not even for a moment.
    setHidden(h, true);
    RECT r{};
    GetWindowRect(h, &r);
    real::SetWindowPos(h, nullptr, offScreenX(r.right - r.left), r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool keepOffScreen(HWND h) { return hideNow() && isHidden(h); }

std::string takeOver() {
    HWND h = gWindow.load();
    if (!h) return "no game window yet";
    gTakenOver       = true;
    backgroundMode() = false;
    syncAudioMute();
    vstate::releaseAll();
    return runOnGameThread(h, kTakeOver);
}

std::string handBack() {
    HWND h = gWindow.load();
    if (!h) return "no game window yet";
    vstate::releaseAll();
    gTakenOver       = false;
    backgroundMode() = true;
    syncAudioMute();
    return runOnGameThread(h, kHandBack);
}

bool hideNewWindows() { return hideNow() && !gCreatedHidden.load(); }

void markHidden(HWND h) {
    SetPropW(h, kHiddenProp, reinterpret_cast<HANDLE>(1));
    gCreatedHidden = true;
}

int offScreenLeft(int windowWidth) { return offScreenX(windowWidth); }

void startWindowWatcher() {
    gInputMsg = RegisterWindowMessageW(L"BeDD.Event");
    gSyncMsg  = RegisterWindowMessageW(L"BeDD.Sync");
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

} // namespace bedd
