#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "ll/api/io/Logger.h"
#include "ll/api/thread/GlobalThreadPauser.h"

// BeDD: drive a LeviLamina Minecraft Bedrock client without touching the real
// mouse, keyboard or foreground window.
//
//   * Window.cpp subclasses the game window. In background mode it swallows the focus-loss
//     messages (so the game never pauses or releases its "focus") and every *real* client-area
//     input message (so the user's mouse and keys never reach the game), and it dispatches
//     synthetic input posted by the control pipe straight into the game's own window procedure.
//   * ApiHooks.cpp hooks the user32 calls through which the game would otherwise grab or read the
//     real devices (ClipCursor, SetCursorPos, GetCursorPos, GetAsyncKeyState, ...): they see the
//     virtual state below instead.
//   * Pipe.cpp serves a JSON-lines control protocol on \\.\pipe\BeDD.
//   * Manager.cpp links the game to the session's manager window (client/BeDDManager.ps1),
//     which shows every headless game and closes them all when it is closed.
namespace bedd {

[[nodiscard]] ll::io::Logger& log();

struct Options {
    /// Shield the game from focus loss and the user's real input, and keep it out of the foreground.
    bool        background = false;
    /// Where the window goes once it exists: "back" (bottom of the Z order), "offscreen", "none".
    std::string placement = "back";
    /// Client-area size to apply once the window exists (0 = leave it alone).
    int clientWidth  = 0;
    int clientHeight = 0;
    /// Log message and API-hook statistics every few seconds.
    bool trace = false;
    /// Control pipe name (\\.\pipe\<name>); one per game instance when several run in parallel.
    std::string pipeName = "BeDD";
    /// In background mode, keep the game on the PC: wildcard bind()s become loopback ones (no
    /// firewall prompts) and servers off the PC are not queried (no bogus "LAN worlds").
    bool loopbackOnly = true;
    /// In background mode, register with the session's manager window when one runs: the game
    /// window then stays off screen and off the taskbar, and the manager shows it instead.
    bool manager = true;
    /// In background mode, mute the game (FMOD master channel group): headless games stay silent.
    bool mute = true;
    /// With "tap": also tap singleplayer worlds and LAN worlds this client hosts (TapLoopback.cpp).
    /// Read as tap && tapLoopback, so it is off unless the tap is on.
    bool tapLoopback = true;
};

/// Read once in load() from <mod>/config/config.json.
[[nodiscard]] Options& options();

/// Live switch for background mode (the "background" op and the Ctrl+Shift+F12 takeover change it).
[[nodiscard]] std::atomic<bool>& backgroundMode();

// ---------------------------------------------------------------------------------------------
// Virtual input state (what the hooked user32 calls report in background mode).
// ---------------------------------------------------------------------------------------------
namespace vstate {
void  setKey(int vk, bool down);
bool  keyDown(int vk);
SHORT keyState(int vk); // GetKeyState() format: 0x8000 = down, 0x0001 = toggled
void  setCursor(POINT client);
POINT cursor(); // client coordinates
void  releaseAll();
} // namespace vstate

// ---------------------------------------------------------------------------------------------
// Real (unhooked) user32 calls, for our own code.
// ---------------------------------------------------------------------------------------------
namespace real {
HWND GetForegroundWindow();
BOOL SetForegroundWindow(HWND h);
BOOL GetCursorPos(LPPOINT p);
SHORT GetAsyncKeyState(int vk);
BOOL  SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags);
} // namespace real

/// Pauses every other thread while a batch of hooks is (un)installed, as LeviLamina's HookRegistrar
/// does (one pause per batch; nothing may be logged while it lasts). The process heap stays locked
/// while the threads stop, so none of them is frozen inside it: the hooking code allocates, and a
/// thread stopped holding the heap lock hung enable() for good.
class HookPause {
public:
    HookPause();
    /// Resume the threads now (before logging what the batch did).
    void end() { mPauser.reset(); }

private:
    std::optional<ll::thread::GlobalThreadPauser> mPauser;
};

/// In background mode already in load(): the game creates and shows its window before enable(),
/// and a managed game's window must be created hidden (CreateWindowExW) since the game does not
/// pump messages until then. Idempotent.
void installApiHooks();
void uninstallApiHooks();
/// "Name=calls ..." of the hooked user32 functions the game has called (trace / "state" op).
[[nodiscard]] std::string apiHookStats();

// ---------------------------------------------------------------------------------------------
// Window (Window.cpp)
// ---------------------------------------------------------------------------------------------
/// The subclassed game window, or nullptr while it does not exist yet.
[[nodiscard]] HWND gameWindow();
[[nodiscard]] bool gameThreadIsCurrent();
/// True while the window procedure handles a synthetic event (on the game thread).
[[nodiscard]] bool inSyntheticDispatch();

void startWindowWatcher();
void stopWindowWatcher();

/// One message for the game's window procedure, dispatched on the game thread in posting order.
struct Event {
    UINT   msg = 0;
    WPARAM wp  = 0;
    LPARAM lp  = 0;
    // Virtual state applied right before dispatch (so GetKeyState etc. agree with the message).
    int  vk     = -1;
    bool vkDown = false;
    bool moveCursor = false;
    POINT cursor{};
    // Synthetic raw mouse input (msg = WM_INPUT): lp is replaced by a handle that the hooked
    // GetRawInputData resolves to this data.
    bool        raw = false;
    RAWMOUSE    rawMouse{};
};

/// Queue events for the game thread. Returns false if there is no window yet.
bool post(Event const& e);
/// Wait until everything posted so far has been dispatched (false on timeout).
bool flush(std::chrono::milliseconds timeout);

/// Raw-input data of a synthetic WM_INPUT handle (nullptr if the handle is a real one).
[[nodiscard]] RAWMOUSE const* syntheticRawMouse(HRAWINPUT h);

/// Size the client area and place the window, never activating it.
std::string applyWindowLayout(int clientW, int clientH, std::string const& placement);
/// Pretend the game just got the focus (WM_ACTIVATEAPP/WM_ACTIVATE/WM_SETFOCUS).
void fakeFocusGained();

/// Linked to the manager window: the game window stays off screen, off the taskbar and out of Alt+Tab.
void setManaged(bool on);
[[nodiscard]] bool managed();
/// A human took the game over from the manager (background off, window on screen and active).
[[nodiscard]] bool takenOver();
/// Hooked ShowWindow, before the game first shows a top-level window: hide it right away when managed.
void prepareFirstShow(HWND h);
/// True while the manager keeps this window off screen (moves by the game are dropped).
[[nodiscard]] bool keepOffScreen(HWND h);
/// Hooked CreateWindowExW: managed games create their windows as hidden tool windows off screen.
[[nodiscard]] bool hideNewWindows();
void               markHidden(HWND h);
[[nodiscard]] int  offScreenLeft(int windowWidth);
/// Manager ops: hand the game to the user's real mouse and keyboard, and take it back.
std::string takeOver();
std::string handBack();

struct Stats {
    std::atomic<uint64_t> synthetic{};
    std::atomic<uint64_t> swallowedInput{};
    std::atomic<uint64_t> swallowedFocus{};
    std::atomic<uint64_t> passed{};
    std::atomic<uint64_t> realForegroundSeen{};
};
[[nodiscard]] Stats& stats();

// ---------------------------------------------------------------------------------------------
// GameInput (GameInputHooks.cpp): Bedrock reads the mouse through GDK GameInput, not window messages.
// ---------------------------------------------------------------------------------------------
void installGameInputHooks();
void uninstallGameInputHooks();
/// GameInput mouse button bits (GameInputMouseButtons).
enum : int { kGiLeft = 0x1, kGiRight = 0x2, kGiMiddle = 0x4 };
/// Move / scroll / press the virtual GameInput mouse (what the game reads in background mode).
void gameInputMouse(int64_t dx, int64_t dy, int wheelNotches, int buttonMask, bool buttonDown);
[[nodiscard]] uint32_t    gameInputButtons();
[[nodiscard]] std::string gameInputStats();

// Audio.cpp: the game is muted in background mode (FMOD).
void installAudioHooks();
void uninstallAudioHooks();
/// Mute or unmute the game as background mode is now (call after changing it).
void                      syncAudioMute();
[[nodiscard]] std::string audioStats();

// NetHooks.cpp: loopback-only network in background mode (no Windows Firewall prompts).
void                   installNetHooks();
void                   uninstallNetHooks();
[[nodiscard]] uint64_t loopbackRebinds();
[[nodiscard]] uint64_t serverQueriesRefused();

/// Trace.cpp: log the game's input events (trace mode only).
void startInputTrace();
void stopInputTrace();

// ---------------------------------------------------------------------------------------------
// Pipe (Pipe.cpp)
// ---------------------------------------------------------------------------------------------
void startPipeServer();
void stopPipeServer();
/// One request line of the control protocol -> one reply line (with the newline).
[[nodiscard]] std::string handleControlLine(std::string const& line);

// ---------------------------------------------------------------------------------------------
// Manager link (Manager.cpp): \\.\pipe\BeDD-Manager-<session id>
// ---------------------------------------------------------------------------------------------
void startManagerLink();
void stopManagerLink();

// ---------------------------------------------------------------------------------------------
// Loopback tap (TapLoopback.cpp): singleplayer and LAN-host worlds, re-serialized.
// ---------------------------------------------------------------------------------------------
void installTapLoopback();
void uninstallTapLoopback();
struct TapLoopbackStats {
    bool     enabled = false;
    int      hooks   = 0;
    int      link    = 0; // tap::Link
    uint64_t clientbound{}, echo{}, bytes{};
    uint64_t unpublished{}; // frames counted while no tap pipe took them
    uint64_t guest{};       // clientbound packets to LAN guests (never published)
};
[[nodiscard]] TapLoopbackStats tapLoopbackStats();

} // namespace bedd
