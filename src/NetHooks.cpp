// Loopback-only network (loopbackOnly): a background test game stays on the PC.
//
// * Windows Firewall asks "allow access?" the first time a program (by full exe path) listens on a
//   non-loopback address. Every clone instance is a new path, so every new test game raised that
//   prompt and stopped unattended runs. A UDP bind() to the wildcard address (0.0.0.0 / ::) is
//   turned into a bind to the loopback address, and so is a TCP bind to a wildcard with a fixed
//   port (a server). A TCP bind to port 0 is an outgoing connection (WinHTTP binds before it
//   connects) and stays as it is: bound to loopback, it could reach the internet only through a
//   proxy on the PC.
// * Bedrock also queries game servers (port 19132) over WinHTTP. Through a proxy on the PC the
//   answer comes from 127.0.0.1, and the server is listed as a "LAN world" at the top of the
//   Worlds tab: a harness that opens the first world joined it instead and hung at "Connecting to
//   multiplayer game" (~97%). Such queries are refused for hosts outside the PC (the game could
//   not play there anyway); servers on the PC still answer.
//
// Installed in load(), before the game sets up its network, and active for the whole process
// (also after the game was handed back to the user: new sockets would raise the prompt then).
#include "Headless.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "ll/api/memory/Hook.h"

namespace headless {

namespace {

using BindFn           = int(WSAAPI*)(SOCKET s, sockaddr const* name, int namelen);
using WinHttpConnectFn = void*(WINAPI*)(void* session, wchar_t const* server, WORD port, DWORD reserved);

BindFn           o_bind           = nullptr;
WinHttpConnectFn o_WinHttpConnect = nullptr;

struct NetHook {
    wchar_t const* module;
    char const*    name;
    void*          detour;
    void**         original;
    void*          target = nullptr;
};

std::atomic<uint64_t> gRebound{}, gServerQueriesRefused{};

int WSAAPI d_bind(SOCKET s, sockaddr const* name, int namelen) {
    if (!name) return o_bind(s, name, namelen);
    int type = 0, typeLen = sizeof(type);
    getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLen);
    if (name->sa_family == AF_INET && namelen >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in a = *reinterpret_cast<sockaddr_in const*>(name);
        if (a.sin_addr.s_addr == htonl(INADDR_ANY) && (type == SOCK_DGRAM || a.sin_port != 0)) {
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            gRebound.fetch_add(1, std::memory_order_relaxed);
            return o_bind(s, reinterpret_cast<sockaddr const*>(&a), sizeof(a));
        }
    } else if (name->sa_family == AF_INET6 && namelen >= static_cast<int>(sizeof(sockaddr_in6))) {
        sockaddr_in6 a = *reinterpret_cast<sockaddr_in6 const*>(name);
        if (IN6_IS_ADDR_UNSPECIFIED(&a.sin6_addr) && (type == SOCK_DGRAM || a.sin6_port != 0)) {
            a.sin6_addr = in6addr_loopback;
            gRebound.fetch_add(1, std::memory_order_relaxed);
            return o_bind(s, reinterpret_cast<sockaddr const*>(&a), sizeof(a));
        }
    }
    return o_bind(s, name, namelen);
}

bool hostOnPc(std::wstring_view host) {
    return host == L"localhost" || host == L"::1" || host == L"[::1]" || host.starts_with(L"127.");
}

void* WINAPI d_WinHttpConnect(void* session, wchar_t const* server, WORD port, DWORD reserved) {
    if (server && port != 80 && port != 443 && !hostOnPc(server)) {
        gServerQueriesRefused.fetch_add(1, std::memory_order_relaxed);
        SetLastError(12029); // ERROR_WINHTTP_CANNOT_CONNECT
        return nullptr;
    }
    return o_WinHttpConnect(session, server, port, reserved);
}

std::array gHooks{
    NetHook{L"ws2_32.dll",  "bind",           reinterpret_cast<void*>(&d_bind),           reinterpret_cast<void**>(&o_bind)          },
    NetHook{L"winhttp.dll", "WinHttpConnect", reinterpret_cast<void*>(&d_WinHttpConnect), reinterpret_cast<void**>(&o_WinHttpConnect)},
};

} // namespace

void installNetHooks() {
    // Resolved before the pause: GetProcAddress takes loader locks a paused thread may hold.
    for (auto& h : gHooks) {
        HMODULE m = GetModuleHandleW(h.module);
        if (!m) m = LoadLibraryW(h.module);
        h.target = m ? reinterpret_cast<void*>(GetProcAddress(m, h.name)) : nullptr;
    }
    std::vector<std::string> failures;
    failures.reserve(gHooks.size());
    {
        HookPause pause;
        for (auto& h : gHooks) {
            if (!h.target
                || ll::memory::hook(h.target, h.detour, reinterpret_cast<ll::memory::FuncPtr*>(h.original), ll::memory::HookPriority::Highest, false)
                       != 0
                || !*h.original) {
                failures.push_back(h.name);
                *h.original = nullptr;
                h.target    = nullptr;
            }
        }
    }
    for (auto const& f : failures) log().error("{} could not be hooked: the game is not limited to loopback (firewall prompts possible)", f);
    log().info("loopback-only network: wildcard binds go to loopback, servers off the PC are not queried");
}

void uninstallNetHooks() {
    HookPause pause;
    for (auto& h : gHooks) {
        if (h.target) ll::memory::unhook(h.target, h.detour, false);
        h.target = nullptr;
    }
}

uint64_t loopbackRebinds() { return gRebound.load(); }

uint64_t serverQueriesRefused() { return gServerQueriesRefused.load(); }

} // namespace headless
