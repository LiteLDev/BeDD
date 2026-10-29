// GameInput (GDK) hooks. Bedrock reads the mouse (and possibly the keyboard) through GameInput
// v2, which talks to the devices directly: neither window messages nor focus are involved, so the
// user's real mouse would still move the camera and click in a background game. In background
// mode IGameInputReading::GetMouseState / GetKeyCount / GetKeyState report the virtual state
// instead (driven by the "raw" op for the mouse, and by synthetic key events for the keyboard).
//
// GameInput has a reading for a mouse only once the device reported input. While nobody touches
// the real mouse (an unattended run), the game's GetCurrentReading fails, it sees no mouse state at
// all and virtual clicks are lost (the cursor still hovers: that goes through window messages). In
// background mode such a miss is answered with a virtual reading of that device instead.
//
// Only the vtable slots used here are declared (GameInput v2, GameInput.h of Microsoft.GameInput
// 2.x): IGameInput 3 GetCurrentTimestamp, 4 GetCurrentReading, 5 GetNextReading,
// 6 GetPreviousReading; IGameInputReading 3 GetInputKind, 4 GetTimestamp, 5 GetDevice,
// 12 GetKeyCount, 13 GetKeyState, 14 GetMouseState.
#include "Headless.h"

#include <array>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ll/api/memory/Hook.h"

namespace headless {

namespace {

bool bg() { return backgroundMode().load(std::memory_order_relaxed); }

// {BBAA66D2-837A-40F7-A303-917D500955F4} = GameInput::v2::IGameInput
constexpr GUID kIID_IGameInputV2{0xbbaa66d2, 0x837a, 0x40f7, {0xa3, 0x03, 0x91, 0x7d, 0x50, 0x09, 0x55, 0xf4}};
constexpr GUID kIID_IUnknown{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr uint32_t kKindKeyboard = 0x10;
constexpr uint32_t kKindMouse    = 0x20;

constexpr HRESULT kDeviceDisconnected = static_cast<HRESULT>(0x838A0001); // GAMEINPUT_E_DEVICE_DISCONNECTED
constexpr HRESULT kReadingNotFound    = static_cast<HRESULT>(0x838A0003); // GAMEINPUT_E_READING_NOT_FOUND

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
using ReleaseFn     = ULONG(STDMETHODCALLTYPE*)(void* self); // IUnknown slot 2 (AddRef, slot 1, alike)
using GetDeviceFn   = void(STDMETHODCALLTYPE*)(void* self, void** device); // IGameInputReading slot 5
using TimestampFn   = uint64_t(STDMETHODCALLTYPE*)(void* self);             // IGameInput slot 3

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

void* gGameInput = nullptr; // our IGameInput (kept alive)

std::once_flag        gReadingHooksOnce;
std::atomic<uint64_t> gMouseReads{}, gKeyReads{}, gVirtualReadings{};

// Virtual GameInput mouse: relative position and wheel accumulate the "raw" op's deltas.
//
// Bedrock polls every mouse device separately, so the virtual state goes to ONE device (the
// "primary": the first one polled, replaced when it has not been polled for a while, e.g. when it
// was unplugged); every other device reports idle (no buttons, no movement). Otherwise a PC with
// two mice would get every virtual click, wheel notch and camera move twice.
//
// Each device starts from the real position/wheel it had when first seen ("base"). The primary
// reports base + (virtual total - total at its last report) and then moves its base there; other
// devices keep reporting their base. So neither the first read nor a change of primary makes the
// camera jump.
std::mutex gMouseMutex;
int64_t    gPosX = 0, gPosY = 0, gWheelX = 0, gWheelY = 0; // virtual totals
uint32_t   gButtons = 0;

struct DeviceMouse {
    int64_t baseX = 0, baseY = 0, baseWheelX = 0, baseWheelY = 0;
    int64_t attachX = 0, attachY = 0, attachWheelX = 0, attachWheelY = 0;
    std::chrono::steady_clock::time_point lastPoll{};
};
std::unordered_map<void*, DeviceMouse> gDevices; // keyed by IGameInputDevice*
void*                                  gPrimary = nullptr;
constexpr auto                         kPrimaryStale = std::chrono::seconds(2);

void attachPrimary(void* device, DeviceMouse& d) {
    gPrimary       = device;
    d.attachX      = gPosX;
    d.attachY      = gPosY;
    d.attachWheelX = gWheelX;
    d.attachWheelY = gWheelY;
}

void** vtableOf(void* obj) { return *reinterpret_cast<void***>(obj); }

// Pauses the other threads once for a batch of hooks (see HookPause); log after it ends.
struct HookBatch {
    HookPause                pause;
    std::vector<std::string> failures;
    void finish() {
        pause.end();
        for (auto const& f : failures) log().error("GameInput: hooking {} failed", f);
        failures.clear();
    }
};

bool hookSlot(HookBatch& batch, void* target, void* detour, void** original, char const* name) {
    if (ll::memory::hook(target, detour, reinterpret_cast<ll::memory::FuncPtr*>(original), ll::memory::HookPriority::Highest, false)
            != 0
        || !*original) {
        batch.failures.emplace_back(name);
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

// Overwrites a mouse state of `device` with the virtual one; `real` = it holds the device's real state.
void virtualMouseState(void* device, bool real, GiMouseState* state) {
    std::lock_guard lock(gMouseMutex);
    auto         now   = std::chrono::steady_clock::now();
    auto [it, isNew]   = gDevices.try_emplace(device);
    DeviceMouse& d     = it->second;
    if (isNew && real) {
        d.baseX      = state->positionX;
        d.baseY      = state->positionY;
        d.baseWheelX = state->wheelX;
        d.baseWheelY = state->wheelY;
    }
    d.lastPoll = now;
    if (!gPrimary) {
        attachPrimary(device, d);
    } else if (gPrimary != device) {
        auto p = gDevices.find(gPrimary);
        if (p == gDevices.end() || now - p->second.lastPoll > kPrimaryStale) {
            // The primary is gone: continue from where this device's idle report left off.
            attachPrimary(device, d);
        }
    }

    bool primary     = gPrimary == device;
    state->buttons   = primary ? gButtons : 0;
    state->positionX = d.baseX + (primary ? gPosX - d.attachX : 0);
    state->positionY = d.baseY + (primary ? gPosY - d.attachY : 0);
    state->wheelX    = d.baseWheelX + (primary ? gWheelX - d.attachWheelX : 0);
    state->wheelY    = d.baseWheelY + (primary ? gWheelY - d.attachWheelY : 0);
    if (primary) {
        // Fold what was reported into the base: a device's base is always the position it last
        // reported, so losing or regaining "primary" never makes the camera jump.
        d.baseX      = state->positionX;
        d.baseY      = state->positionY;
        d.baseWheelX = state->wheelX;
        d.baseWheelY = state->wheelY;
        attachPrimary(device, d);
    }
    if (HWND h = gameWindow()) {
        POINT p = vstate::cursor();
        ClientToScreen(h, &p);
        state->absolutePositionX = p.x;
        state->absolutePositionY = p.y;
    }
}

bool STDMETHODCALLTYPE d_GetMouseState(void* self, GiMouseState* state) {
    gMouseReads.fetch_add(1, std::memory_order_relaxed);
    bool ok = o_GetMouseState(self, state);
    if (!bg() || !state) return ok;

    void* device = nullptr;
    reinterpret_cast<GetDeviceFn>(vtableOf(self)[5])(self, &device);
    if (device) reinterpret_cast<ReleaseFn>(vtableOf(device)[2])(device); // only used as a key
    virtualMouseState(device, ok, state);
    return true;
}

// A mouse reading of our own (an IGameInputReading as far as the game is concerned): the virtual
// mouse state of one device. Every slot not set below reports "nothing" (0 / false).
struct VirtualReading {
    void* const*       vtable;
    std::atomic<ULONG> refs;
    void*              device; // holds a reference
    uint64_t           timestamp;
};

uint64_t STDMETHODCALLTYPE vrNothing(void*) { return 0; }

HRESULT STDMETHODCALLTYPE vrQueryInterface(void* self, GUID const& iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != kIID_IUnknown) return E_NOINTERFACE;
    static_cast<VirtualReading*>(self)->refs.fetch_add(1);
    *out = self;
    return S_OK;
}

ULONG STDMETHODCALLTYPE vrAddRef(void* self) { return static_cast<VirtualReading*>(self)->refs.fetch_add(1) + 1; }

ULONG STDMETHODCALLTYPE vrRelease(void* self) {
    auto* r = static_cast<VirtualReading*>(self);
    ULONG n = r->refs.fetch_sub(1) - 1;
    if (n == 0) {
        reinterpret_cast<ReleaseFn>(vtableOf(r->device)[2])(r->device);
        delete r;
    }
    return n;
}

uint32_t STDMETHODCALLTYPE vrGetInputKind(void*) { return kKindMouse; }

uint64_t STDMETHODCALLTYPE vrGetTimestamp(void* self) { return static_cast<VirtualReading*>(self)->timestamp; }

void STDMETHODCALLTYPE vrGetDevice(void* self, void** device) {
    auto* r = static_cast<VirtualReading*>(self);
    reinterpret_cast<ReleaseFn>(vtableOf(r->device)[1])(r->device); // AddRef
    *device = r->device;
}

bool STDMETHODCALLTYPE vrGetMouseState(void* self, GiMouseState* state) {
    gMouseReads.fetch_add(1, std::memory_order_relaxed);
    if (!state) return false;
    *state = {};
    virtualMouseState(static_cast<VirtualReading*>(self)->device, false, state);
    return true;
}

auto const gVirtualReadingVtable = [] {
    std::array<void*, 32> vt;
    vt.fill(reinterpret_cast<void*>(&vrNothing));
    vt[0]  = reinterpret_cast<void*>(&vrQueryInterface);
    vt[1]  = reinterpret_cast<void*>(&vrAddRef);
    vt[2]  = reinterpret_cast<void*>(&vrRelease);
    vt[3]  = reinterpret_cast<void*>(&vrGetInputKind);
    vt[4]  = reinterpret_cast<void*>(&vrGetTimestamp);
    vt[5]  = reinterpret_cast<void*>(&vrGetDevice);
    vt[14] = reinterpret_cast<void*>(&vrGetMouseState);
    return vt;
}();

bool isVirtualReading(void* reading) { return reading && *static_cast<void**>(reading) == gVirtualReadingVtable.data(); }

// The game asked for a mouse reading GameInput does not have: hand it a virtual one.
bool virtualReadingFor(HRESULT hr, uint32_t kind, void* device, void** reading) {
    if (SUCCEEDED(hr) || hr == kDeviceDisconnected || kind != kKindMouse || !device || !reading || !bg()) return false;
    if (gVirtualReadings.fetch_add(1) == 0) {
        log().info(
            "GameInput: no mouse reading yet (0x{:08X}); the game reads the virtual mouse until the real one reports",
            static_cast<uint32_t>(hr)
        );
    }
    reinterpret_cast<ReleaseFn>(vtableOf(device)[1])(device); // AddRef
    uint64_t now = gGameInput ? reinterpret_cast<TimestampFn>(vtableOf(gGameInput)[3])(gGameInput) : 0;
    *reading     = new VirtualReading{gVirtualReadingVtable.data(), 1, device, now};
    return true;
}

void hookReadingVtable(void* reading) {
    std::call_once(gReadingHooksOnce, [reading] {
        void** vt       = vtableOf(reading);
        t_GetKeyCount   = vt[12];
        t_GetKeyState   = vt[13];
        t_GetMouseState = vt[14];
        int       n     = 0;
        HookBatch batch;
        n += hookSlot(batch, t_GetKeyCount, reinterpret_cast<void*>(&d_GetKeyCount), reinterpret_cast<void**>(&o_GetKeyCount), "GetKeyCount");
        n += hookSlot(batch, t_GetKeyState, reinterpret_cast<void*>(&d_GetKeyState), reinterpret_cast<void**>(&o_GetKeyState), "GetKeyState");
        n += hookSlot(batch, t_GetMouseState, reinterpret_cast<void*>(&d_GetMouseState), reinterpret_cast<void**>(&o_GetMouseState), "GetMouseState");
        batch.finish();
        log().info("GameInput: IGameInputReading hooks installed: {}/3", n);
    });
}

HRESULT STDMETHODCALLTYPE d_GetCurrentReading(void* self, uint32_t kind, void* device, void** reading) {
    HRESULT hr = o_GetCurrentReading(self, kind, device, reading);
    if (SUCCEEDED(hr) && reading && *reading) hookReadingVtable(*reading);
    else if (virtualReadingFor(hr, kind, device, reading)) hr = S_OK;
    return hr;
}

// A virtual reading is the only one there is: GameInput cannot step from it.
HRESULT STDMETHODCALLTYPE d_GetNextReading(void* self, void* ref, uint32_t kind, void* device, void** reading) {
    if (isVirtualReading(ref)) {
        if (reading) *reading = nullptr;
        return kReadingNotFound;
    }
    HRESULT hr = o_GetNextReading(self, ref, kind, device, reading);
    if (SUCCEEDED(hr) && reading && *reading) hookReadingVtable(*reading);
    return hr;
}

HRESULT STDMETHODCALLTYPE d_GetPreviousReading(void* self, void* ref, uint32_t kind, void* device, void** reading) {
    if (isVirtualReading(ref)) {
        if (reading) *reading = nullptr;
        return kReadingNotFound;
    }
    HRESULT hr = o_GetPreviousReading(self, ref, kind, device, reading);
    if (SUCCEEDED(hr) && reading && *reading) hookReadingVtable(*reading);
    return hr;
}

void unhookIf(void*& target, void* detour) {
    if (target) ll::memory::unhook(target, detour, false);
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
    gGameInput           = gi;
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
    {
        HookBatch batch;
        hookSlot(batch, t_GetCurrentReading, reinterpret_cast<void*>(&d_GetCurrentReading), reinterpret_cast<void**>(&o_GetCurrentReading), "GetCurrentReading");
        hookSlot(batch, t_GetNextReading, reinterpret_cast<void*>(&d_GetNextReading), reinterpret_cast<void**>(&o_GetNextReading), "GetNextReading");
        hookSlot(batch, t_GetPreviousReading, reinterpret_cast<void*>(&d_GetPreviousReading), reinterpret_cast<void**>(&o_GetPreviousReading), "GetPreviousReading");
        batch.finish();
    }
    // Our IGameInput instance stays alive on purpose (the hooks point into its DLL).
    log().info("GameInput: v2 hooks installed");
}

void uninstallGameInputHooks() {
    HookPause pause;
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
    size_t devices = 0;
    {
        std::lock_guard lock(gMouseMutex);
        devices = gDevices.size();
    }
    return std::format(
        "mouseReads={} keyReads={} mouseDevices={} hooked={} virtualReadings={}",
        gMouseReads.load(),
        gKeyReads.load(),
        devices,
        o_GetMouseState != nullptr,
        gVirtualReadings.load()
    );
}

} // namespace headless
