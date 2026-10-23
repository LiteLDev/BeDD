// Loopback-only sockets: Windows Firewall asks "allow access?" the first time a program (by full
// exe path) listens on a non-loopback address. Every clone instance is a new path, so every new
// test game raised that prompt and stopped unattended runs. In background mode, bind() to the
// wildcard address (0.0.0.0 / ::) is turned into a bind to the loopback address: the game can
// still reach local servers and the internet (outgoing connections), but nothing outside the PC
// can reach it, and the firewall never asks. Installed in load(), before the game sets up its
// network.
#include "Headless.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#include "ll/api/memory/Hook.h"

namespace headless {

namespace {

using BindFn = int(WSAAPI*)(SOCKET s, sockaddr const* name, int namelen);

BindFn                o_bind = nullptr;
void*                 t_bind = nullptr;
std::atomic<uint64_t> gRebound{};

int WSAAPI d_bind(SOCKET s, sockaddr const* name, int namelen) {
    if (backgroundMode().load(std::memory_order_relaxed) && options().loopbackOnly && name) {
        if (name->sa_family == AF_INET && namelen >= static_cast<int>(sizeof(sockaddr_in))) {
            sockaddr_in a = *reinterpret_cast<sockaddr_in const*>(name);
            if (a.sin_addr.s_addr == htonl(INADDR_ANY)) {
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                gRebound.fetch_add(1, std::memory_order_relaxed);
                return o_bind(s, reinterpret_cast<sockaddr const*>(&a), sizeof(a));
            }
        } else if (name->sa_family == AF_INET6 && namelen >= static_cast<int>(sizeof(sockaddr_in6))) {
            sockaddr_in6 a = *reinterpret_cast<sockaddr_in6 const*>(name);
            if (IN6_IS_ADDR_UNSPECIFIED(&a.sin6_addr)) {
                a.sin6_addr = in6addr_loopback;
                gRebound.fetch_add(1, std::memory_order_relaxed);
                return o_bind(s, reinterpret_cast<sockaddr const*>(&a), sizeof(a));
            }
        }
    }
    return o_bind(s, name, namelen);
}

} // namespace

void installNetHooks() {
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryW(L"ws2_32.dll");
    t_bind = ws2 ? reinterpret_cast<void*>(GetProcAddress(ws2, "bind")) : nullptr;
    if (!t_bind
        || ll::memory::hook(t_bind, reinterpret_cast<void*>(&d_bind), reinterpret_cast<ll::memory::FuncPtr*>(&o_bind), ll::memory::HookPriority::Highest)
               != 0
        || !o_bind) {
        log().error("ws2_32!bind could not be hooked: sockets are not limited to loopback (firewall prompts possible)");
        t_bind = nullptr;
        o_bind = nullptr;
        return;
    }
    log().info("ws2_32!bind hooked: wildcard binds go to loopback in background mode");
}

void uninstallNetHooks() {
    if (t_bind) ll::memory::unhook(t_bind, reinterpret_cast<void*>(&d_bind));
    t_bind = nullptr;
}

uint64_t loopbackRebinds() { return gRebound.load(); }

} // namespace headless
