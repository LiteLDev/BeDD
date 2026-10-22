// user32 hooks: in background mode the game reads the virtual input state and can neither grab
// the real cursor nor take the foreground. Outside background mode every hook passes through.
#include "Headless.h"

#include <array>
#include <string_view>

#include "ll/api/memory/Hook.h"

namespace headless {

namespace {

bool bg() { return backgroundMode().load(std::memory_order_relaxed); }

struct HookSlot {
    char const*           name;
    void*                 detour;
    void**                original;
    void*                 target = nullptr;
    std::atomic<uint64_t> calls{};
};

// One original-function pointer per hook. Null until installed: real:: then calls user32 directly.
#define HI_ORIG(fn) inline decltype(&::fn) o_##fn = nullptr

HI_ORIG(ClipCursor);
HI_ORIG(SetCursorPos);
HI_ORIG(GetCursorPos);
HI_ORIG(GetPhysicalCursorPos);
HI_ORIG(GetAsyncKeyState);
HI_ORIG(GetKeyState);
HI_ORIG(GetKeyboardState);
HI_ORIG(GetForegroundWindow);
HI_ORIG(GetActiveWindow);
HI_ORIG(GetFocus);
HI_ORIG(SetForegroundWindow);
HI_ORIG(GetRawInputData);
HI_ORIG(GetRawInputBuffer);
HI_ORIG(ShowWindow);
HI_ORIG(SetWindowPos);
HI_ORIG(SetActiveWindow);
HI_ORIG(BringWindowToTop);

#undef HI_ORIG

HookSlot* slotFor(std::string_view name);

#define HI_COUNT(name)                                                                                                 \
    static HookSlot* slot_ = slotFor(name);                                                                            \
    if (slot_) slot_->calls.fetch_add(1, std::memory_order_relaxed)

BOOL WINAPI d_ClipCursor(RECT const* r) {
    HI_COUNT("ClipCursor");
    if (bg()) return TRUE; // never confine the real cursor
    return o_ClipCursor(r);
}

BOOL WINAPI d_SetCursorPos(int x, int y) {
    HI_COUNT("SetCursorPos");
    if (bg()) {
        // The game re-centres the cursor while the mouse is grabbed: move the virtual one only.
        if (HWND h = gameWindow()) {
            POINT p{x, y};
            ScreenToClient(h, &p);
            vstate::setCursor(p);
        }
        return TRUE;
    }
    return o_SetCursorPos(x, y);
}

BOOL virtualCursorScreen(LPPOINT p) {
    HWND h = gameWindow();
    if (!h || !p) return FALSE;
    *p = vstate::cursor();
    ClientToScreen(h, p);
    return TRUE;
}

BOOL WINAPI d_GetCursorPos(LPPOINT p) {
    HI_COUNT("GetCursorPos");
    if (bg() && virtualCursorScreen(p)) return TRUE;
    return o_GetCursorPos(p);
}

BOOL WINAPI d_GetPhysicalCursorPos(LPPOINT p) {
    HI_COUNT("GetPhysicalCursorPos");
    if (bg() && virtualCursorScreen(p)) return TRUE;
    return o_GetPhysicalCursorPos(p);
}

SHORT WINAPI d_GetAsyncKeyState(int vk) {
    HI_COUNT("GetAsyncKeyState");
    if (bg()) return vstate::keyDown(vk) ? static_cast<SHORT>(0x8000) : 0;
    return o_GetAsyncKeyState(vk);
}

SHORT WINAPI d_GetKeyState(int vk) {
    HI_COUNT("GetKeyState");
    if (bg()) return vstate::keyState(vk);
    return o_GetKeyState(vk);
}

BOOL WINAPI d_GetKeyboardState(PBYTE keys) {
    HI_COUNT("GetKeyboardState");
    if (bg() && keys) {
        for (int vk = 0; vk < 256; ++vk) {
            SHORT s  = vstate::keyState(vk);
            keys[vk] = static_cast<BYTE>(((s & 0x8000) ? 0x80 : 0) | (s & 0x01));
        }
        return TRUE;
    }
    return o_GetKeyboardState(keys);
}

HWND WINAPI d_GetForegroundWindow() {
    HI_COUNT("GetForegroundWindow");
    if (bg()) {
        if (HWND h = gameWindow()) return h;
    }
    return o_GetForegroundWindow();
}

HWND WINAPI d_GetActiveWindow() {
    HI_COUNT("GetActiveWindow");
    if (bg() && gameThreadIsCurrent()) {
        if (HWND h = gameWindow()) return h;
    }
    return o_GetActiveWindow();
}

HWND WINAPI d_GetFocus() {
    HI_COUNT("GetFocus");
    if (bg() && gameThreadIsCurrent()) {
        if (HWND h = gameWindow()) return h;
    }
    return o_GetFocus();
}

// A top-level window of this process (the game window, also before Window.cpp has found it).
bool ownTopLevel(HWND h) {
    if (!h || GetAncestor(h, GA_ROOT) != h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

BOOL WINAPI d_SetForegroundWindow(HWND h) {
    HI_COUNT("SetForegroundWindow");
    if (bg() && ownTopLevel(h)) return TRUE; // the game stays in the background
    return o_SetForegroundWindow(h);
}

BOOL WINAPI d_ShowWindow(HWND h, int cmd) {
    HI_COUNT("ShowWindow");
    if (bg() && ownTopLevel(h)) {
        // Show without activating (the start-up show would otherwise take the foreground).
        switch (cmd) {
        case SW_SHOWNORMAL:
        case SW_SHOWDEFAULT:
        case SW_RESTORE: cmd = SW_SHOWNOACTIVATE; break;
        case SW_SHOW:
        case SW_SHOWMAXIMIZED: cmd = SW_SHOWNA; break;
        case SW_SHOWMINIMIZED: cmd = SW_SHOWMINNOACTIVE; break;
        default: break;
        }
    }
    return o_ShowWindow(h, cmd);
}

BOOL WINAPI d_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    HI_COUNT("SetWindowPos");
    if (bg() && ownTopLevel(h)) {
        flags |= SWP_NOACTIVATE;
        if (after == HWND_TOP || after == HWND_TOPMOST) flags |= SWP_NOZORDER; // stay behind the user's windows
    }
    return o_SetWindowPos(h, after, x, y, cx, cy, flags);
}

HWND WINAPI d_SetActiveWindow(HWND h) {
    HI_COUNT("SetActiveWindow");
    if (bg() && ownTopLevel(h)) return gameWindow() ? gameWindow() : h;
    return o_SetActiveWindow(h);
}

BOOL WINAPI d_BringWindowToTop(HWND h) {
    HI_COUNT("BringWindowToTop");
    if (bg() && ownTopLevel(h)) return TRUE;
    return o_BringWindowToTop(h);
}

UINT WINAPI d_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT headerSize) {
    HI_COUNT("GetRawInputData");
    if (RAWMOUSE const* m = syntheticRawMouse(h)) {
        if (!size || headerSize != sizeof(RAWINPUTHEADER)) return static_cast<UINT>(-1);
        RAWINPUT ri{};
        ri.header.dwType  = RIM_TYPEMOUSE;
        ri.header.dwSize  = sizeof(RAWINPUT);
        ri.header.hDevice = nullptr;
        ri.header.wParam  = RIM_INPUT;
        ri.data.mouse     = *m;
        UINT need         = cmd == RID_HEADER ? sizeof(RAWINPUTHEADER) : sizeof(RAWINPUT);
        if (!data) {
            *size = need;
            return 0;
        }
        if (*size < need) {
            *size = need;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return static_cast<UINT>(-1);
        }
        memcpy(data, &ri, need);
        return need;
    }
    return o_GetRawInputData(h, cmd, data, size, headerSize);
}

UINT WINAPI d_GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize) {
    HI_COUNT("GetRawInputBuffer");
    UINT r = o_GetRawInputBuffer(data, size, headerSize);
    // Buffered raw input is read straight from the queue: drop the real devices' data.
    if (bg() && data && r != static_cast<UINT>(-1)) return 0;
    return r;
}

#undef HI_COUNT

#define HI_SLOT(fn) HookSlot{#fn, reinterpret_cast<void*>(&d_##fn), reinterpret_cast<void**>(&o_##fn)}

std::array<HookSlot, 17> gSlots{
    HI_SLOT(ClipCursor),
    HI_SLOT(SetCursorPos),
    HI_SLOT(GetCursorPos),
    HI_SLOT(GetPhysicalCursorPos),
    HI_SLOT(GetAsyncKeyState),
    HI_SLOT(GetKeyState),
    HI_SLOT(GetKeyboardState),
    HI_SLOT(GetForegroundWindow),
    HI_SLOT(GetActiveWindow),
    HI_SLOT(GetFocus),
    HI_SLOT(SetForegroundWindow),
    HI_SLOT(GetRawInputData),
    HI_SLOT(GetRawInputBuffer),
    HI_SLOT(ShowWindow),
    HI_SLOT(SetWindowPos),
    HI_SLOT(SetActiveWindow),
    HI_SLOT(BringWindowToTop),
};

#undef HI_SLOT

HookSlot* slotFor(std::string_view name) {
    for (auto& s : gSlots)
        if (name == s.name) return &s;
    return nullptr;
}

} // namespace

void installApiHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    int installed = 0;
    for (auto& s : gSlots) {
        s.target = reinterpret_cast<void*>(GetProcAddress(user32, s.name));
        if (!s.target) {
            log().error("user32!{} not found; not hooked", s.name);
            continue;
        }
        int rc = ll::memory::hook(
            s.target,
            s.detour,
            reinterpret_cast<ll::memory::FuncPtr*>(s.original),
            ll::memory::HookPriority::Highest
        );
        if (rc != 0 || !*s.original) {
            log().error("hooking user32!{} failed ({})", s.name, rc);
            *s.original = nullptr;
            s.target    = nullptr;
            continue;
        }
        ++installed;
    }
    log().info("user32 hooks installed: {}/{}", installed, gSlots.size());
}

void uninstallApiHooks() {
    for (auto& s : gSlots) {
        if (!s.target) continue;
        ll::memory::unhook(s.target, s.detour);
        s.target    = nullptr;
        *s.original = nullptr;
    }
}

std::string apiHookStats() {
    std::string out;
    for (auto& s : gSlots) {
        uint64_t n = s.calls.load();
        if (!n) continue;
        if (!out.empty()) out += ' ';
        out += s.name;
        out += '=';
        out += std::to_string(n);
    }
    return out.empty() ? "none" : out;
}

namespace real {
HWND GetForegroundWindow() { return o_GetForegroundWindow ? o_GetForegroundWindow() : ::GetForegroundWindow(); }
BOOL SetForegroundWindow(HWND h) { return o_SetForegroundWindow ? o_SetForegroundWindow(h) : ::SetForegroundWindow(h); }
BOOL GetCursorPos(LPPOINT p) { return o_GetCursorPos ? o_GetCursorPos(p) : ::GetCursorPos(p); }
SHORT GetAsyncKeyState(int vk) { return o_GetAsyncKeyState ? o_GetAsyncKeyState(vk) : ::GetAsyncKeyState(vk); }
BOOL  SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    return o_SetWindowPos ? o_SetWindowPos(h, after, x, y, cx, cy, flags) : ::SetWindowPos(h, after, x, y, cx, cy, flags);
}
} // namespace real

} // namespace headless
