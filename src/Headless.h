#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "ll/api/io/Logger.h"

// LeviHeadlessInput: drive a LeviLamina Minecraft Bedrock client without touching the real
// mouse, keyboard or foreground window.
//
//   * Window.cpp subclasses the game window. In background mode it swallows the focus-loss
//     messages (so the game never pauses or releases its "focus") and every *real* client-area
//     input message (so the user's mouse and keys never reach the game), and it dispatches
//     synthetic input posted by the control pipe straight into the game's own window procedure.
//   * ApiHooks.cpp hooks the user32 calls through which the game would otherwise grab or read the
//     real devices (ClipCursor, SetCursorPos, GetCursorPos, GetAsyncKeyState, ...): they see the
//     virtual state below instead.
//   * Pipe.cpp serves a JSON-lines control protocol on \\.\pipe\LeviHeadlessInput.
namespace headless {

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
} // namespace real

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

struct Stats {
    std::atomic<uint64_t> synthetic{};
    std::atomic<uint64_t> swallowedInput{};
    std::atomic<uint64_t> swallowedFocus{};
    std::atomic<uint64_t> passed{};
    std::atomic<uint64_t> realForegroundSeen{};
};
[[nodiscard]] Stats& stats();

// ---------------------------------------------------------------------------------------------
// Pipe (Pipe.cpp)
// ---------------------------------------------------------------------------------------------
void startPipeServer();
void stopPipeServer();

} // namespace headless
