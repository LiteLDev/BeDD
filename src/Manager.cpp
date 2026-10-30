// Link to the manager window of this logon session (client/BeDDManager.ps1) on
// \\.\pipe\BeDD-Manager-<session id>. The game says hello once, then answers the
// manager's requests: every control-pipe op, plus "close". The game window always stays off screen
// and off the taskbar (Window.cpp) and the manager is the only place that shows it; the manager also
// puts the game into a kill-on-close job, so closing the manager closes the game.
//
// A game never runs without the manager: with none running, the mod starts it (config
// managerScript, which Install-BeDDMod writes); with none linked in time, or once the link is lost
// for good, the game closes.
#include "BeDD.h"

#include <format>
#include <thread>

#include <nlohmann/json.hpp>

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

constexpr int kLinkProtocol = 1;
// How long a game waits for its first link (the manager may be starting, the game loading).
constexpr auto kFirstLinkTimeout = 90s;

std::jthread gLink;

// The session's manager, or the one BEDD_MANAGER_PIPE names (e.g. a recording's own).
std::wstring managerPipePath() {
    wchar_t buf[256]{};
    DWORD   n = GetEnvironmentVariableW(L"BEDD_MANAGER_PIPE", buf, 256);
    if (n > 0 && n < 256) return L"\\\\.\\pipe\\" + std::wstring(buf, n);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return std::format(L"\\\\.\\pipe\\BeDD-Manager-{}", session);
}

std::string utf8(std::wstring const& w) {
    if (w.empty()) return {};
    int         n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The game instance: the folder of Minecraft.Windows.exe (versions\<instance>).
std::string instanceName() {
    wchar_t buf[MAX_PATH]{};
    DWORD   n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    auto         slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash);
    slash = path.find_last_of(L"\\/");
    return utf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

// Who started the game (Start-BeDDGame.ps1 sets it: the project and worktree of the caller).
std::string label() {
    wchar_t buf[512]{};
    DWORD   n = GetEnvironmentVariableW(L"BEDD_LABEL", buf, 512);
    return n > 0 && n < 512 ? utf8(std::wstring(buf, n)) : std::string();
}

HANDLE connect() {
    std::wstring path = managerPipePath();
    for (int attempt = 0; attempt < 2; ++attempt) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        // Every instance busy for a moment (another game is connecting): wait for the next one.
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(path.c_str(), 500)) return nullptr;
    }
    return nullptr;
}

bool writeLine(HANDLE h, std::string const& line) {
    DWORD written = 0;
    return WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size();
}

bool sayHello(HANDLE h) {
    json hello{
        {"op",         "hello"                                    },
        {"protocol",   kLinkProtocol                              },
        {"pid",        GetCurrentProcessId()                      },
        {"hwnd",       reinterpret_cast<uintptr_t>(gameWindow())  },
        {"pipe",       options().pipeName                         },
        {"instance",   instanceName()                             },
        {"label",      label()                                    },
    };
    return writeLine(h, hello.dump() + "\n");
}

// The manager's answer to hello: only then is the game linked (a manager that is closing hangs up
// without one).
bool welcomed(HANDLE h) {
    std::string line;
    for (int waited = 0; waited < 3000;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (!avail) {
            std::this_thread::sleep_for(10ms);
            waited += 10;
            continue;
        }
        char  c = 0;
        DWORD n = 0;
        if (!ReadFile(h, &c, 1, &n, nullptr) || n == 0) return false;
        if (c == '\n') {
            json reply = json::parse(line, nullptr, false);
            return reply.is_object() && reply.value("ok", false);
        }
        line += c;
    }
    return false;
}

// Connected, said hello and welcomed; nullptr when no manager takes the game.
HANDLE link() {
    HANDLE h = connect();
    if (h && sayHello(h) && welcomed(h)) return h;
    if (h) CloseHandle(h);
    return nullptr;
}

std::string handleManagerLine(std::string const& line) {
    json req = json::parse(line, nullptr, false);
    if (req.is_object()) {
        std::string op = req.value("op", "");
        json        r{
            {"ok", true}
        };
        if (op == "close") {
            HWND h = gameWindow();
            if (!h || !PostMessageW(h, WM_CLOSE, 0, 0)) r = {{"ok", false}, {"error", "no game window to close"}};
            else log().info("the manager window asked the game to close");
        } else {
            return handleControlLine(line);
        }
        if (req.contains("id")) r["id"] = req["id"];
        return r.dump() + "\n";
    }
    return handleControlLine(line);
}

// Answers the manager's requests until it goes away.
void serve(HANDLE pipe, std::stop_token const& stop) {
    std::string buf;
    char        chunk[4096];
    while (!stop.stop_requested()) {
        DWORD n = 0;
        if (!ReadFile(pipe, chunk, sizeof(chunk), &n, nullptr) || n == 0) return;
        buf.append(chunk, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!writeLine(pipe, handleManagerLine(line))) return;
        }
    }
}

bool sleepUnlessStopped(std::stop_token const& stop, std::chrono::milliseconds ms) {
    for (auto t = 0ms; t < ms && !stop.stop_requested(); t += 50ms) std::this_thread::sleep_for(50ms);
    return !stop.stop_requested();
}

// pwsh -File <managerScript> -Ensure, which starts the manager outside this process tree.
void startManager() {
    std::string const& script = options().managerScript;
    if (script.empty()) {
        log().error("no manager window running and no managerScript in config.json to start one");
        return;
    }
    int          n = MultiByteToWideChar(CP_UTF8, 0, script.data(), static_cast<int>(script.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, script.data(), static_cast<int>(script.size()), wide.data(), n);
    std::wstring        cmd = L"pwsh.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + wide + L"\" -Ensure";
    STARTUPINFOW        si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        log().error("could not start the manager window ({}): error {}", script, GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log().info("no manager window running: starting it ({})", script);
}

// A game the manager does not show must not run at all.
[[noreturn]] void closeUnmanaged(char const* why) {
    log().error("{}: closing the game (a BeDD game only runs inside the manager window)", why);
    TerminateProcess(GetCurrentProcess(), 1);
    ExitProcess(1);
}

void run(std::stop_token stop, HANDLE first) {
    HANDLE pipe = first;
    if (!pipe) {
        startManager();
        for (auto waited = 0ms; !pipe && waited < kFirstLinkTimeout; waited += 500ms) {
            if (!sleepUnlessStopped(stop, 500ms)) return;
            pipe = link();
        }
        if (!pipe) closeUnmanaged("no manager window linked within 90 s");
    }
    bool again = false;
    while (!stop.stop_requested()) {
        log().info("linked to the manager window{}", again ? " again" : "");
        setManaged(true);
        serve(pipe, stop);
        CloseHandle(pipe);
        pipe = nullptr;
        if (stop.stop_requested()) break;
        // Lost the link: link again right away if the manager is still there (it dropped the link
        // after a slow answer).
        for (int attempt = 0; attempt < 12 && !pipe && sleepUnlessStopped(stop, 250ms); ++attempt) pipe = link();
        if (stop.stop_requested()) break;
        // The manager closed without closing the game (it normally does).
        if (!pipe) closeUnmanaged("the manager window went away");
        again = true;
    }
    if (pipe) CloseHandle(pipe);
}

} // namespace

void startManagerLink() {
    // The first attempt runs right here, before the game creates its window.
    HANDLE first = link();
    if (first) setManaged(true);
    gLink = std::jthread(run, first);
}

void stopManagerLink() {
    if (!gLink.joinable()) return;
    gLink.request_stop();
    // Unblock a ReadFile on the link (again, in case the thread only got to it after a cancel).
    HANDLE thread = gLink.native_handle();
    for (int i = 0; i < 100 && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT; ++i) {
        CancelSynchronousIo(thread);
        WaitForSingleObject(thread, 50);
    }
    gLink.join();
}

} // namespace bedd
