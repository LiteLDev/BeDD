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
// * allowHosts (Remote mode: a server off the PC) lets the game reach those hosts and no others:
//   they are queried over WinHTTP, and an outgoing UDP socket (a wildcard bind to port 0) is not
//   bound to loopback (a server off the PC could not answer it) but implicitly, by connecting it
//   towards the first allowed host: the firewall prompts for explicit binds only. Listeners (a
//   fixed port: LAN hosting) stay on loopback, and datagrams (sendto, WSASendTo, and a UDP connect
//   for send/WSASend) to any other host off the PC are refused, LAN broadcasts included. Without allowHosts, or when all of them are on
//   the PC, none of this is installed.
//
// Installed in load(), before the game sets up its network, and active for the whole process
// (also after the game was handed back to the user: new sockets would raise the prompt then).
//
// In a Bedrock Dedicated Server (the server target) the same hooks keep the server on the PC: its
// RakNet ports (IPv4 and IPv6), LAN discovery and NetherNet. There a UDP bind (or a TCP bind with a
// fixed port) to any address off loopback goes to loopback, not only one to the wildcard: NetherNet
// (WebRTC) binds each network interface's own address, which would raise the prompt as well.
#include "BeDD.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ll/api/memory/Hook.h"

namespace bedd {

namespace {

using BindFn           = int(WSAAPI*)(SOCKET s, sockaddr const* name, int namelen);
using WinHttpConnectFn = void*(WINAPI*)(void* session, wchar_t const* server, WORD port, DWORD reserved);
using SendToFn         = int(WSAAPI*)(SOCKET s, char const* buf, int len, int flags, sockaddr const* to, int tolen);
using WSASendToFn      = int(WSAAPI*)(
    SOCKET                             s,
    LPWSABUF                           buffers,
    DWORD                              count,
    LPDWORD                            sent,
    DWORD                              flags,
    sockaddr const*                    to,
    int                                tolen,
    LPWSAOVERLAPPED                    overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion
);

using ConnectFn        = int(WSAAPI*)(SOCKET s, sockaddr const* name, int namelen);

BindFn           o_bind           = nullptr;
ConnectFn        o_connect        = nullptr;
WinHttpConnectFn o_WinHttpConnect = nullptr;
SendToFn         o_sendto         = nullptr;
WSASendToFn      o_WSASendTo      = nullptr;

// allowHosts, resolved once in installNetHooks() before the hooks go in (read-only afterwards).
std::vector<std::wstring>     gAllowNames; // as configured and their addresses, lower case
std::vector<sockaddr_storage> gAllowAddrs; // their addresses off the PC (port ignored), as a name resolved at load
std::optional<sockaddr_storage> gRoute4;  // the first allowed IPv4 address off the PC
std::optional<sockaddr_storage> gRoute6;  // likewise for IPv6

struct NetHook {
    wchar_t const* module;
    char const*    name;
    void*          detour;
    void**         original;
    void*          target = nullptr;
};

std::atomic<uint64_t> gRebound{}, gServerQueriesRefused{}, gSendsRefused{}, gImplicitBinds{};

// Instead of an explicit bind (which the firewall prompts for): connect the UDP socket to `route`,
// which binds it implicitly (an ephemeral port on the PC's address towards it; no prompt), then
// dissolve the association again so it sends to any address (sendto/WSASendTo still filter).
int implicitBind(SOCKET s, sockaddr_storage const& route) {
    auto conn = o_connect ? o_connect : &connect;
    int  len  = route.ss_family == AF_INET ? static_cast<int>(sizeof(sockaddr_in)) : static_cast<int>(sizeof(sockaddr_in6));
    if (conn(s, reinterpret_cast<sockaddr const*>(&route), len) != 0) return SOCKET_ERROR;
    sockaddr_storage none{};
    none.ss_family = route.ss_family;
    // The zero address removes the default peer. Should it fail, the socket still reaches the allowed host.
    if (conn(s, reinterpret_cast<sockaddr const*>(&none), len) != 0)
        log().warn("allowHosts: an outgoing UDP socket stays connected to the allowed host (error {})", WSAGetLastError());
    gImplicitBinds.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// The route of an IPv6 socket: the allowed IPv6 host, else the IPv4 one as ::ffff:a.b.c.d when the
// socket is dual-stack (IPV6_V6ONLY off).
std::optional<sockaddr_storage> route6(SOCKET s) {
    if (gRoute6) return gRoute6;
    DWORD v6only = 1;
    int   optLen = sizeof(v6only);
    if (!gRoute4 || getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), &optLen) != 0 || v6only) return std::nullopt;
    auto const&      v4 = reinterpret_cast<sockaddr_in const&>(*gRoute4);
    sockaddr_storage r{};
    auto&            a = reinterpret_cast<sockaddr_in6&>(r);
    a.sin6_family      = AF_INET6;
    a.sin6_port        = v4.sin_port;
    a.sin6_addr.u.Byte[10] = a.sin6_addr.u.Byte[11] = 0xff;
    std::memcpy(&a.sin6_addr.u.Byte[12], &v4.sin_addr, sizeof(v4.sin_addr));
    return r;
}

#ifdef LL_PLAT_S
constexpr bool kAnyAddress = true; // also binds to an interface's own address (see above)
#else
constexpr bool kAnyAddress = false;
#endif

bool rebindV4(in_addr const& a) {
    return a.s_addr == htonl(INADDR_ANY) || (kAnyAddress && (ntohl(a.s_addr) >> 24) != 127);
}

bool v4MappedLoopback(in6_addr const& a) { return IN6_IS_ADDR_V4MAPPED(&a) && a.u.Byte[12] == 127; }

bool rebindV6(in6_addr const& a) {
    return IN6_IS_ADDR_UNSPECIFIED(&a) || (kAnyAddress && !IN6_IS_ADDR_LOOPBACK(&a) && !v4MappedLoopback(a));
}

// The loopback address for an IPv6 socket. A dual-stack one (IPV6_V6ONLY off: BDS serves IPv4 and
// IPv6 on server-port that way) takes IPv4's, ::ffff:127.0.0.1, on the server: on ::1 it would
// lose the IPv4 clients, and those are the ones that join 127.0.0.1.
in6_addr loopbackV6(SOCKET s) {
    if (kAnyAddress) {
        DWORD v6only = 1;
        int   len    = sizeof(v6only);
        if (getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), &len) == 0 && !v6only) {
            in6_addr mapped{};
            mapped.u.Byte[10] = 0xff;
            mapped.u.Byte[11] = 0xff;
            mapped.u.Byte[12] = 127;
            mapped.u.Byte[15] = 1;
            return mapped;
        }
    }
    return in6addr_loopback;
}

std::string addressText(sockaddr const* a) {
    char        buf[INET6_ADDRSTRLEN]{};
    void const* in = a->sa_family == AF_INET ? static_cast<void const*>(&reinterpret_cast<sockaddr_in const*>(a)->sin_addr)
                                             : static_cast<void const*>(&reinterpret_cast<sockaddr_in6 const*>(a)->sin6_addr);
    return inet_ntop(a->sa_family, in, buf, sizeof(buf)) ? buf : "?";
}

// Trace mode: what a bind asked for, and how the rebind to loopback went (WSA error on failure).
void logRebind(int type, sockaddr const* asked, unsigned port, int result) {
    int err = result == 0 ? 0 : WSAGetLastError();
    log().info("bind {} {}:{} -> loopback: {}", type == SOCK_DGRAM ? "udp" : "tcp", addressText(asked), port, result == 0 ? "ok" : "error " + std::to_string(err));
    WSASetLastError(err);
}

int WSAAPI d_bind(SOCKET s, sockaddr const* name, int namelen) {
    if (!name) return o_bind(s, name, namelen);
    int type = 0, typeLen = sizeof(type);
    getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLen);
    if (name->sa_family == AF_INET && namelen >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in a = *reinterpret_cast<sockaddr_in const*>(name);
        if (rebindV4(a.sin_addr) && (type == SOCK_DGRAM || a.sin_port != 0)) {
            // An outgoing UDP socket (port 0) is bound implicitly towards an allowed host off the PC
            // (allowHosts); everything else, listeners (a fixed port: LAN hosting) included, on loopback.
            bool out = type == SOCK_DGRAM && a.sin_port == 0 && gRoute4;
            if (out) {
                if (options().trace) log().info("bind udp {}:0: implicit, towards allowHosts", addressText(name));
                return implicitBind(s, *gRoute4);
            }
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            gRebound.fetch_add(1, std::memory_order_relaxed);
            int r = o_bind(s, reinterpret_cast<sockaddr const*>(&a), sizeof(a));
            if (options().trace) logRebind(type, name, ntohs(a.sin_port), r);
            return r;
        }
    } else if (name->sa_family == AF_INET6 && namelen >= static_cast<int>(sizeof(sockaddr_in6))) {
        sockaddr_in6 a = *reinterpret_cast<sockaddr_in6 const*>(name);
        if (rebindV6(a.sin6_addr) && (type == SOCK_DGRAM || a.sin6_port != 0)) {
            auto route = type == SOCK_DGRAM && a.sin6_port == 0 ? route6(s) : std::nullopt;
            if (route) {
                if (options().trace) log().info("bind udp [{}]:0: implicit, towards allowHosts", addressText(name));
                return implicitBind(s, *route);
            }
            a.sin6_addr = loopbackV6(s);
            gRebound.fetch_add(1, std::memory_order_relaxed);
            int r = o_bind(s, reinterpret_cast<sockaddr const*>(&a), sizeof(a));
            // 127.0.0.1 at that port may be an IPv4 socket's already: then ::1.
            if (r != 0 && WSAGetLastError() == WSAEADDRINUSE && !IN6_IS_ADDR_LOOPBACK(&a.sin6_addr)) {
                a.sin6_addr = in6addr_loopback;
                r           = o_bind(s, reinterpret_cast<sockaddr const*>(&a), sizeof(a));
            }
            if (options().trace) logRebind(type, name, ntohs(a.sin6_port), r);
            return r;
        }
    }
    return o_bind(s, name, namelen);
}

bool hostOnPc(std::wstring_view host) {
    return host == L"localhost" || host == L"::1" || host == L"[::1]" || host.starts_with(L"127.");
}

std::wstring lower(std::wstring_view s) {
    std::wstring r(s);
    for (auto& c : r) c = static_cast<wchar_t>(std::towlower(c));
    return r;
}

bool hostAllowed(std::wstring_view host) {
    if (host.size() > 2 && host.front() == L'[' && host.back() == L']') host = host.substr(1, host.size() - 2);
    return std::ranges::find(gAllowNames, lower(host)) != gAllowNames.end();
}

// A datagram to `to` may leave: loopback, an allowed host, or not IP at all. IPv4-mapped IPv6 is IPv4.
bool addressAllowed(sockaddr const* to, int tolen) {
    in_addr v4{};
    if (to->sa_family == AF_INET && tolen >= static_cast<int>(sizeof(sockaddr_in))) {
        v4 = reinterpret_cast<sockaddr_in const*>(to)->sin_addr;
    } else if (to->sa_family == AF_INET6 && tolen >= static_cast<int>(sizeof(sockaddr_in6))) {
        in6_addr const& a = reinterpret_cast<sockaddr_in6 const*>(to)->sin6_addr;
        if (!IN6_IS_ADDR_V4MAPPED(&a)) {
            if (IN6_IS_ADDR_LOOPBACK(&a)) return true;
            return std::ranges::any_of(gAllowAddrs, [&](sockaddr_storage const& s) {
                return s.ss_family == AF_INET6 && IN6_ADDR_EQUAL(&reinterpret_cast<sockaddr_in6 const&>(s).sin6_addr, &a);
            });
        }
        std::memcpy(&v4, &a.u.Byte[12], sizeof(v4));
    } else {
        return true;
    }
    if ((ntohl(v4.s_addr) >> 24) == 127) return true;
    return std::ranges::any_of(gAllowAddrs, [&](sockaddr_storage const& s) {
        return s.ss_family == AF_INET && reinterpret_cast<sockaddr_in const&>(s).sin_addr.s_addr == v4.s_addr;
    });
}

// A connected UDP socket sends with send/WSASend: filter its peer instead (TCP is left alone). The
// zero address only removes a peer.
int WSAAPI d_connect(SOCKET s, sockaddr const* name, int namelen) {
    int type = 0, typeLen = sizeof(type);
    getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLen);
    if (type == SOCK_DGRAM && name && !addressAllowed(name, namelen)) {
        bool zero = (name->sa_family == AF_INET && reinterpret_cast<sockaddr_in const*>(name)->sin_addr.s_addr == htonl(INADDR_ANY))
                 || (name->sa_family == AF_INET6 && IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<sockaddr_in6 const*>(name)->sin6_addr));
        if (!zero) {
            gSendsRefused.fetch_add(1, std::memory_order_relaxed);
            WSASetLastError(WSAEHOSTUNREACH);
            return SOCKET_ERROR;
        }
    }
    return o_connect(s, name, namelen);
}

int WSAAPI d_sendto(SOCKET s, char const* buf, int len, int flags, sockaddr const* to, int tolen) {
    if (to && !addressAllowed(to, tolen)) {
        gSendsRefused.fetch_add(1, std::memory_order_relaxed);
        WSASetLastError(WSAEHOSTUNREACH);
        return SOCKET_ERROR;
    }
    return o_sendto(s, buf, len, flags, to, tolen);
}

int WSAAPI d_WSASendTo(
    SOCKET                             s,
    LPWSABUF                           buffers,
    DWORD                              count,
    LPDWORD                            sent,
    DWORD                              flags,
    sockaddr const*                    to,
    int                                tolen,
    LPWSAOVERLAPPED                    overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion
) {
    if (to && !addressAllowed(to, tolen)) {
        gSendsRefused.fetch_add(1, std::memory_order_relaxed);
        WSASetLastError(WSAEHOSTUNREACH);
        return SOCKET_ERROR;
    }
    return o_WSASendTo(s, buffers, count, sent, flags, to, tolen, overlapped, completion);
}

void* WINAPI d_WinHttpConnect(void* session, wchar_t const* server, WORD port, DWORD reserved) {
    if (server && port != 80 && port != 443 && !hostOnPc(server) && !hostAllowed(server)) {
        gServerQueriesRefused.fetch_add(1, std::memory_order_relaxed);
        SetLastError(12029); // ERROR_WINHTTP_CANNOT_CONNECT
        return nullptr;
    }
    return o_WinHttpConnect(session, server, port, reserved);
}

std::array gHooks{
    NetHook{L"ws2_32.dll",  "bind",           reinterpret_cast<void*>(&d_bind),           reinterpret_cast<void**>(&o_bind)          },
    NetHook{L"winhttp.dll", "WinHttpConnect", reinterpret_cast<void*>(&d_WinHttpConnect), reinterpret_cast<void**>(&o_WinHttpConnect)},
    // The last kSendHooks: only with an allowed host off the PC.
    NetHook{L"ws2_32.dll",  "sendto",         reinterpret_cast<void*>(&d_sendto),         reinterpret_cast<void**>(&o_sendto)        },
    NetHook{L"ws2_32.dll",  "WSASendTo",      reinterpret_cast<void*>(&d_WSASendTo),      reinterpret_cast<void**>(&o_WSASendTo)     },
    NetHook{L"ws2_32.dll",  "connect",        reinterpret_cast<void*>(&d_connect),        reinterpret_cast<void**>(&o_connect)       },
};
constexpr size_t kSendHooks = 3;


// allowHosts -> gAllowNames, gAllowAddrs, gRoute4/gRoute6. Before the hooks go in.
void resolveAllowHosts() {
    if (options().allowHosts.empty()) return;
    WSADATA wsa{};
    bool    started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    if (!started) log().error("allowHosts: WSAStartup failed; the hosts are only queried (WinHTTP), not reached over UDP");
    for (auto const& host : options().allowHosts) {
        gAllowNames.push_back(lower(std::wstring(host.begin(), host.end())));
        addrinfo  hints{.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM};
        addrinfo* list = nullptr;
        if (!started || getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || !list) {
            log().warn("allowHosts: {} does not resolve; it is not reachable over UDP", host);
            continue;
        }
        std::string addrs;
        for (addrinfo* a = list; a; a = a->ai_next) {
            if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
            int len = static_cast<int>(a->ai_addrlen);
            if (addressAllowed(a->ai_addr, len)) continue; // on the PC, or listed already
            sockaddr_storage s{};
            std::memcpy(&s, a->ai_addr, a->ai_addrlen);
            gAllowAddrs.push_back(s);
            std::string text = addressText(a->ai_addr);
            gAllowNames.push_back(std::wstring(text.begin(), text.end()));
            addrs += (addrs.empty() ? "" : ", ") + text;
            auto& route = a->ai_family == AF_INET ? gRoute4 : gRoute6;
            if (!route) {
                reinterpret_cast<sockaddr_in&>(s).sin_port = htons(19132); // any port: only the route matters (same offset in IPv6)
                route                                      = s;
                log().info("allowHosts: outgoing UDP sockets are bound implicitly towards {} ({})", host, text);
            }
        }
        freeaddrinfo(list);
        log().info("allowHosts: {} is {}", host, addrs.empty() ? std::string("on the PC") : addrs);
    }
    if (started) WSACleanup();
}

} // namespace

#ifdef LL_PLAT_S // the client build has it in ApiHooks.cpp (keep the two the same)
HookPause::HookPause() {
    if (!ll::memory::shouldHookSuspendThreads()) return;
    HANDLE heap   = GetProcessHeap();
    bool   locked = HeapLock(heap) != FALSE;
    mPauser.emplace();
    if (locked) HeapUnlock(heap);
}
#endif

void installNetHooks() {
    resolveAllowHosts();
    size_t const count = gAllowAddrs.empty() ? gHooks.size() - kSendHooks : gHooks.size();
    // Resolved before the pause: GetProcAddress takes loader locks a paused thread may hold.
    for (size_t i = 0; i < count; ++i) {
        auto&   h = gHooks[i];
        HMODULE m = GetModuleHandleW(h.module);
        if (!m) m = LoadLibraryW(h.module);
        h.target = m ? reinterpret_cast<void*>(GetProcAddress(m, h.name)) : nullptr;
    }
    std::vector<std::string> failures;
    failures.reserve(gHooks.size());
    {
        HookPause pause;
        for (size_t i = 0; i < count; ++i) {
            auto& h = gHooks[i];
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

uint64_t sendsRefused() { return gSendsRefused.load(); }

} // namespace bedd
