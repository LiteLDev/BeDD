// GameInput (GDK) hooks. Bedrock reads the mouse (and possibly the keyboard) through GameInput
// v2, which talks to the devices directly: neither window messages nor focus are involved, so the
// user's real mouse would still move the camera and click in a background game. In background
// mode IGameInputReading::GetMouseState / GetKeyCount / GetKeyState report the virtual state
// instead (driven by the "raw" op for the mouse, and by synthetic key events for the keyboard).
//
// Only the vtable slots used here are declared (GameInput v2, GameInput.h of Microsoft.GameInput
// 2.x): IGameInput 4 GetCurrentReading, 5 GetNextReading, 6 GetPreviousReading;
// IGameInputReading 12 GetKeyCount, 13 GetKeyState, 14 GetMouseState.
#include "Headless.h"

#include <format>
#include <mutex>

#include "ll/api/memory/Hook.h"

namespace headless {

namespace {

bool bg() { return backgroundMode().load(std::memory_order_relaxed); }

// {BBAA66D2-837A-40F7-A303-917D500955F4} = GameInput::v2::IGameInput
constexpr GUID kIID_IGameInputV2{0xbbaa66d2, 0x837a, 0x40f7, {0xa3, 0x03, 0x91, 0x7d, 0x50, 0x09, 0x55, 0xf4}};

constexpr uint32_t kKindKeyboard = 0x10;
constexpr uint32_t kKindMouse    = 0x20;

struct GiMouseState {
    uint32_t buttons;
    uint32_t positions;
    int64_t  positionX;
    int64_t  positionY;
    int64_t  absolutePositionX;
    int64_t  absolutePositionY;
    int64_t  wheelX;
    int64_t  wheelY;
};

struct GiKeyState {
    uint32_t scanCode;
    uint32_t codePoint;
    uint8_t  virtualKey;
    bool     isDeadKey;
};

using GetReadingFn  = HRESULT(STDMETHODCALLTYPE*)(void* self, uint32_t kind, void* device, void** reading);
using GetNextFn     = HRESULT(STDMETHODCALLTYPE*)(void* self, void* ref, uint32_t kind, void* device, void** reading);
using GetKeyCountFn = uint32_t(STDMETHODCALLTYPE*)(void* self);
using GetKeyStateFn = uint32_t(STDMETHODCALLTYPE*)(void* self, uint32_t count, GiKeyState* states);
using GetMouseFn    = bool(STDMETHODCALLTYPE*)(void* self, GiMouseState* state);
using ReleaseFn     = ULONG(STDMETHODCALLTYPE*)(void* self);

GetReadingFn  o_GetCurrentReading  = nullptr;
GetNextFn     o_GetNextReading     = nullptr;
GetNextFn     o_GetPreviousReading = nullptr;
GetKeyCountFn o_GetKeyCount        = nullptr;
GetKeyStateFn o_GetKeyState        = nullptr;
GetMouseFn    o_GetMouseState      = nullptr;

void* t_GetCurrentReading  = nullptr;
void* t_GetNextReading     = nullptr;
void* t_GetPreviousReading = nullptr;
void* t_GetKeyCount        = nullptr;
void* t_GetKeyState        = nullptr;
void* t_GetMouseState      = nullptr;

std::once_flag        gReadingHooksOnce;
std::atomic<uint64_t> gMouseReads{}, gKeyReads{};

// Virtual GameInput mouse: relative position and wheel accumulate the "raw" op's deltas on top of
// where the real device was when background mode first read it (so the camera does not jump).
std::mutex gMouseMutex;
bool       gMouseInit = false;
int64_t    gPosX = 0, gPosY = 0, gWheelX = 0, gWheelY = 0;
uint32_t   gButtons = 0;

void** vtableOf(void* obj) { return *reinterpret_cast<void***>(obj); }

bool hookSlot(void* target, void* detour, void** original, char const* name) {
    if (ll::memory::hook(target, detour, reinterpret_cast<ll::memory::FuncPtr*>(original), ll::memory::HookPriority::Highest)
            != 0
        || !*original) {
        log().error("GameInput: hooking {} failed", name);
        *original = nullptr;
        return false;
    }
    return true;
}

uint32_t STDMETHODCALLTYPE d_GetKeyCount(void* self) {
    gKeyReads.fetch_add(1, std::memory_order_relaxed);
    if (!bg()) return o_GetKeyCount(self);
    uint32_t n = 0;
    for (int vk = 8; vk < 256; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue; // GameInput reports the L/R keys
        if (vstate::keyDown(vk)) ++n;
    }
    return n;
}

uint32_t STDMETHODCALLTYPE d_GetKeyState(void* self, uint32_t count, GiKeyState* states) {
    if (!bg()) return o_GetKeyState(self, count, states);
    uint32_t n = 0;
    for (int vk = 8; vk < 256 && n < count; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || !vstate::keyDown(vk)) continue;
        GiKeyState& s = states[n++];
        s.scanCode    = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
        s.codePoint   = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFFFFFF;
        s.virtualKey  = static_cast<uint8_t>(vk);
        s.isDeadKey   = false;
    }
    return n;
}

bool STDMETHODCALLTYPE d_GetMouseState(void* self, GiMouseState* state) {
    gMouseReads.fetch_add(1, std::memory_order_relaxed);
    bool ok = o_GetMouseState(self, state);
    if (!bg() || !state) return ok;
    std::lock_guard lock(gMouseMutex);
    if (!gMouseInit) {
        if (ok) {
            gPosX   = state->positionX;
            gPosY   = state->positionY;
            gWheelX = state->wheelX;
            gWheelY = state->wheelY;
        }
        gMouseInit = true;
    }
    state->buttons   = gButtons;
    state->positionX = gPosX;
    state->positionY = gPosY;
    state->wheelX    = gWheelX;
    state->wheelY    = gWheelY;
    if (HWND h = gameWindow()) {
        POINT p = vstate::cursor();
        ClientToScreen(h, &p);
        state->absolutePositionX = p.x;
        state->absolutePositionY = p.y;
    }
    return true;
}

void hookReadingVtable(void* reading) {
    std::call_once(gReadingHooksOnce, [reading] {
        void** vt       = vtableOf(reading);
        t_GetKeyCount   = vt[12];
        t_GetKeyState   = vt[13];
        t_GetMouseState = vt[14];
        int n           = 0;
        n += hookSlot(t_GetKeyCount, reinterpret_cast<void*>(&d_GetKeyCount), reinterpret_cast<void**>(&o_GetKeyCount), "GetKeyCount");
        n += hookSlot(t_GetKeyState, reinterpret_cast<void*>(&d_GetKeyState), reinterpret_cast<void**>(&o_GetKeyState), "GetKeyState");
        n += hookSlot(t_GetMouseState, reinterpret_cast<void*>(&d_GetMouseState), reinterpret_cast<void**>(&o_GetMouseState), "GetMouseState");
        log().info("GameInput: IGameInputReading hooks installed: {}/3", n);
    });
}

HRESULT STDMETHODCALLTYPE d_GetCurrentReading(void* self, uint32_t kind, void* device, void** reading) {
    HRESULT hr = o_GetCurrentReading(self, kind, device, reading);
    if (SUCCEEDED(hr) && reading && *reading) hookReadingVtable(*reading);
    return hr;
}

HRESULT STDMETHODCALLTYPE d_GetNextReading(void* self, void* ref, uint32_t kind, void* device, void** reading) {
    HRESULT hr = o_GetNextReading(self, ref, kind, device, reading);
    if (SUCCEEDED(hr) && reading && *reading) hookReadingVtable(*reading);
    return hr;
}

HRESULT STDMETHODCALLTYPE d_GetPreviousReading(void* self, void* ref, uint32_t kind, void* device, void** reading) {
    HRESULT hr = o_GetPreviousReading(self, ref, kind, device, reading);
    if (SUCCEEDED(hr) && reading && *reading) hookReadingVtable(*reading);
    return hr;
}

void unhookIf(void*& target, void* detour) {
    if (target) ll::memory::unhook(target, detour);
    target = nullptr;
}

} // namespace

void installGameInputHooks() {
    HMODULE m = GetModuleHandleW(L"GameInputRedist.dll");
    if (!m) m = LoadLibraryW(L"GameInputRedist.dll");
    if (!m) {
        log().warn("GameInput: GameInputRedist.dll not available; GameInput is not shielded");
        return;
    }
    using CreateFn = HRESULT(WINAPI*)(void**);
    using QueryFn  = HRESULT(STDMETHODCALLTYPE*)(void* self, GUID const& iid, void** out);
    auto  create   = reinterpret_cast<CreateFn>(GetProcAddress(m, "GameInputCreate"));
    void* unk      = nullptr;
    if (!create || FAILED(create(&unk)) || !unk) {
        log().warn("GameInput: GameInputCreate failed; GameInput is not shielded");
        return;
    }
    void* gi = nullptr;
    HRESULT hr = reinterpret_cast<QueryFn>(vtableOf(unk)[0])(unk, kIID_IGameInputV2, &gi);
    reinterpret_cast<ReleaseFn>(vtableOf(unk)[2])(unk);
    if (FAILED(hr) || !gi) {
        log().warn("GameInput: no v2 IGameInput (0x{:08X}); GameInput is not shielded", static_cast<uint32_t>(hr));
        return;
    }
    void** vt            = vtableOf(gi);
    t_GetCurrentReading  = vt[4];
    t_GetNextReading     = vt[5];
    t_GetPreviousReading = vt[6];
    // Find the reading class right away when a device already has a reading; otherwise the first
    // reading the game asks for installs the reading hooks.
    for (uint32_t kind : {kKindMouse, kKindKeyboard}) {
        void* reading = nullptr;
        if (SUCCEEDED(reinterpret_cast<GetReadingFn>(t_GetCurrentReading)(gi, kind, nullptr, &reading)) && reading) {
            hookReadingVtable(reading);
            reinterpret_cast<ReleaseFn>(vtableOf(reading)[2])(reading);
            break;
        }
    }
    hookSlot(t_GetCurrentReading, reinterpret_cast<void*>(&d_GetCurrentReading), reinterpret_cast<void**>(&o_GetCurrentReading), "GetCurrentReading");
    hookSlot(t_GetNextReading, reinterpret_cast<void*>(&d_GetNextReading), reinterpret_cast<void**>(&o_GetNextReading), "GetNextReading");
    hookSlot(t_GetPreviousReading, reinterpret_cast<void*>(&d_GetPreviousReading), reinterpret_cast<void**>(&o_GetPreviousReading), "GetPreviousReading");
    // Our IGameInput instance stays alive on purpose (the hooks point into its DLL).
    log().info("GameInput: v2 hooks installed");
}

void uninstallGameInputHooks() {
    unhookIf(t_GetCurrentReading, reinterpret_cast<void*>(&d_GetCurrentReading));
    unhookIf(t_GetNextReading, reinterpret_cast<void*>(&d_GetNextReading));
    unhookIf(t_GetPreviousReading, reinterpret_cast<void*>(&d_GetPreviousReading));
    unhookIf(t_GetKeyCount, reinterpret_cast<void*>(&d_GetKeyCount));
    unhookIf(t_GetKeyState, reinterpret_cast<void*>(&d_GetKeyState));
    unhookIf(t_GetMouseState, reinterpret_cast<void*>(&d_GetMouseState));
}

void gameInputMouse(int64_t dx, int64_t dy, int wheelNotches, int buttonMask, bool buttonDown) {
    std::lock_guard lock(gMouseMutex);
    gPosX += dx;
    gPosY += dy;
    gWheelY += static_cast<int64_t>(wheelNotches) * WHEEL_DELTA;
    if (buttonMask) gButtons = buttonDown ? (gButtons | buttonMask) : (gButtons & ~static_cast<uint32_t>(buttonMask));
}

uint32_t gameInputButtons() {
    std::lock_guard lock(gMouseMutex);
    return gButtons;
}

std::string gameInputStats() {
    return std::format("mouseReads={} keyReads={} hooked={}", gMouseReads.load(), gKeyReads.load(), o_GetMouseState != nullptr);
}

} // namespace headless
