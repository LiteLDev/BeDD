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
// * allowHosts "*" (any Remote server): the same, with the hosts added as `join` names them
//   (allowHostNow). Until the first one, outgoing UDP sockets are bound towards a documentation
//   address (192.0.2.1: the default route's interface; nothing is sent there); each added host
//   becomes the route for the sockets bound after it.
//
// Installed in load(), before the game sets up its network, and active for the whole process
// (also after the game was handed back to the user: new sockets would raise the prompt then).
//
// In a Bedrock Dedicated Server (the server target) the same hooks keep the server on the PC: its
// RakNet ports (IPv4 and IPv6), LAN discovery and NetherNet. A UDP bind (or a TCP bind with a fixed
// port) to any address off loopback goes to loopback, not only one to the wildcard: NetherNet
// (WebRTC) binds each network interface's own address, which would raise the prompt as well. The
// game does the same since 1.26.60, where it joins servers over NetherNet, unless allowHosts lets
// it reach a host off the PC. A server on loopback publishes 127.0.0.1 as the address clients send
// to (server-udp-ports, Initialize-BeDDServer); its checks to the client's interface addresses go
// nowhere, and the client's from loopback answer as peer-reflexive candidates.
#include "BeDD.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <atomic>
#include <optional>
#include <shared_mutex>
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

// allowHosts, resolved in installNetHooks() before the hooks go in; with allowHosts "*", `join`
// adds hosts later (allowHostNow). gAllowLock guards these: the hooks read them on the game's
// network threads.
std::shared_mutex               gAllowLock;
std::vector<std::wstring>       gAllowNames; // as configured and their addresses, lower case
std::vector<sockaddr_storage>   gAllowAddrs; // their addresses off the PC (port ignored), resolved when added
std::optional<sockaddr_storage> gRoute4;     // the first allowed IPv4 address off the PC (with "*": the latest joined)
std::optional<sockaddr_storage> gRoute6;     // likewise for IPv6
bool                            gRoute4Placeholder = false; // gRoute4 is 192.0.2.1 (allowHosts "*", no host yet)
// The game may reach a host off the PC: an allowed address off it, or allowHosts "*". Read without the lock.
std::atomic<bool> gReachOff{false};

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
std::optional<sockaddr_storage> route6(SOCKET s, std::optional<sockaddr_storage> const& r4, std::optional<sockaddr_storage> const& r6) {
    if (r6) return r6;
    DWORD v6only = 1;
    int   optLen = sizeof(v6only);
    if (!r4 || getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), &optLen) != 0 || v6only) return std::nullopt;
    auto const&      v4 = reinterpret_cast<sockaddr_in const&>(*r4);
    sockaddr_storage r{};
    auto&            a = reinterpret_cast<sockaddr_in6&>(r);
    a.sin6_family      = AF_INET6;
    a.sin6_port        = v4.sin_port;
    a.sin6_addr.u.Byte[10] = a.sin6_addr.u.Byte[11] = 0xff;
    std::memcpy(&a.sin6_addr.u.Byte[12], &v4.sin_addr, sizeof(v4.sin_addr));
    return r;
}

// Binds to an interface's own address go to loopback too (see above): always in a server, and in
// the game unless it may reach a host off the PC (allowHosts), which NetherNet does from them.
bool anyAddress() {
#ifdef LL_PLAT_S
    return true;
#else
    return !gReachOff.load(std::memory_order_relaxed);
#endif
}

bool rebindV4(in_addr const& a) {
    return a.s_addr == htonl(INADDR_ANY) || (anyAddress() && (ntohl(a.s_addr) >> 24) != 127);
}

bool v4MappedLoopback(in6_addr const& a) { return IN6_IS_ADDR_V4MAPPED(&a) && a.u.Byte[12] == 127; }

bool rebindV6(in6_addr const& a) {
    return IN6_IS_ADDR_UNSPECIFIED(&a) || (anyAddress() && !IN6_IS_ADDR_LOOPBACK(&a) && !v4MappedLoopback(a));
}

// The loopback address for an IPv6 socket. A dual-stack one (IPV6_V6ONLY off) takes IPv4's,
// ::ffff:127.0.0.1: on ::1 it could reach no IPv4 address. BDS serves IPv4 and IPv6 on server-port
// that way and would lose the IPv4 clients, the ones that join 127.0.0.1; and since 1.26.60 the
// game joins over NetherNet from such a socket, which on ::1 never reached a server at 127.0.0.1.
in6_addr loopbackV6(SOCKET s) {
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
    std::optional<sockaddr_storage> r4, r6;
    bool                            placeholder = false;
    {
        std::shared_lock lock(gAllowLock);
        r4          = gRoute4;
        r6          = gRoute6;
        placeholder = gRoute4Placeholder;
    }
    if (name->sa_family == AF_INET && namelen >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in a = *reinterpret_cast<sockaddr_in const*>(name);
        if (rebindV4(a.sin_addr) && (type == SOCK_DGRAM || a.sin_port != 0)) {
            // An outgoing UDP socket (port 0) is bound implicitly towards an allowed host off the PC
            // (allowHosts); everything else, listeners (a fixed port: LAN hosting) included, on loopback.
            bool out = type == SOCK_DGRAM && a.sin_port == 0 && r4;
            if (out) {
                if (options().trace) log().info("bind udp {}:0: implicit, towards allowHosts", addressText(name));
                if (implicitBind(s, *r4) == 0) return 0;
                // The placeholder route ("*", no host yet) may have no interface (offline): loopback, as without it.
                if (!placeholder) return SOCKET_ERROR;
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
            auto route = type == SOCK_DGRAM && a.sin6_port == 0 ? route6(s, r4, r6) : std::nullopt;
            if (route) {
                if (options().trace) log().info("bind udp [{}]:0: implicit, towards allowHosts", addressText(name));
                if (implicitBind(s, *route) == 0) return 0;
                if (!placeholder || r6) return SOCKET_ERROR;
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
    auto             name = lower(host);
    std::shared_lock lock(gAllowLock);
    return std::ranges::find(gAllowNames, name) != gAllowNames.end();
}

// A datagram to `to` may leave: loopback, an allowed host, or not IP at all. IPv4-mapped IPv6 is IPv4.
// gAllowLock held (either way).
bool addressAllowedLocked(sockaddr const* to, int tolen) {
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

bool addressAllowed(sockaddr const* to, int tolen) {
    std::shared_lock lock(gAllowLock);
    return addressAllowedLocked(to, tolen);
}

// Adds what `host` resolved to (`list`) off the PC to the allowed addresses and names. The first
// address of a family becomes its route (and with `latest`, as allowHosts "*" joins, the host's first
// address replaces it). Returns the addresses off the PC, comma separated. gAllowLock held exclusively.
std::string addAddressesLocked(std::string const& host, addrinfo const* list, bool latest) {
    std::string addrs;
    bool        routed4 = false, routed6 = false;
    for (addrinfo const* a = list; a; a = a->ai_next) {
        if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
        sockaddr_storage s{};
        std::memcpy(&s, a->ai_addr, a->ai_addrlen);
        bool onPc = a->ai_family == AF_INET ? (ntohl(reinterpret_cast<sockaddr_in const&>(s).sin_addr.s_addr) >> 24) == 127
                                            : IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<sockaddr_in6 const&>(s).sin6_addr);
        if (onPc) continue;
        std::string text = addressText(a->ai_addr);
        if (!addressAllowedLocked(a->ai_addr, static_cast<int>(a->ai_addrlen))) {
            gAllowAddrs.push_back(s);
            std::wstring wtext(text.begin(), text.end());
            if (std::ranges::find(gAllowNames, wtext) == gAllowNames.end()) gAllowNames.push_back(std::move(wtext)); // a literal is its own name
            addrs += (addrs.empty() ? "" : ", ") + text;
        } else if (!latest) {
            continue; // listed already
        }
        bool  v4     = a->ai_family == AF_INET;
        auto& route  = v4 ? gRoute4 : gRoute6;
        bool& routed = v4 ? routed4 : routed6;
        if (routed) continue;
        if (!route || (v4 && gRoute4Placeholder) || latest) {
            reinterpret_cast<sockaddr_in&>(s).sin_port = htons(19132); // any port: only the route matters (same offset in IPv6)
            route                                      = s;
            if (v4) gRoute4Placeholder = false;
            log().info("allowHosts: outgoing UDP sockets are bound implicitly towards {} ({})", host, text);
        }
        routed = true;
    }
    if (!gAllowAddrs.empty()) gReachOff = true;
    return addrs;
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
    std::unique_lock lock(gAllowLock);
#ifndef LL_PLAT_S
    if (options().allowAnyHost) {
        sockaddr_in r{};
        r.sin_family = AF_INET;
        r.sin_port   = htons(19132);
        inet_pton(AF_INET, "192.0.2.1", &r.sin_addr);
        sockaddr_storage s{};
        std::memcpy(&s, &r, sizeof(r));
        gRoute4            = s;
        gRoute4Placeholder = true;
        gReachOff          = true;
        log().info("allowHosts \"*\": `join` lets the game reach its host; until the first, outgoing UDP takes the default route");
    }
#endif
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
        std::string addrs = addAddressesLocked(host, list, false);
        freeaddrinfo(list);
        log().info("allowHosts: {} is {}", host, addrs.empty() ? std::string("on the PC") : addrs);
    }
    if (started) WSACleanup();
}

} // namespace

void installNetHooks() {
    resolveAllowHosts();
    size_t const count = gReachOff ? gHooks.size() : gHooks.size() - kSendHooks;
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

bool allowHostNow(std::string const& host, std::string& error) {
    std::string name = host;
    if (name.size() > 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);
    std::wstring wide = lower(std::wstring(name.begin(), name.end()));
    if (hostOnPc(wide) || hostAllowed(wide)) return true; // on the PC, configured, or added by an earlier join
    if (!options().allowAnyHost) {
        error = host + " is off the PC and not in allowHosts (loopbackOnly): use Remote mode (BeDD.exe mod config <instance>"
                       " --world-mode Remote --server-host <host>, or --server-host * for any server)";
        return false;
    }
    // Resolved outside the lock: the hooks keep running meanwhile.
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup failed: " + host + " cannot be resolved";
        return false;
    }
    addrinfo  hints{.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM};
    addrinfo* list = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &list) != 0 || !list) {
        WSACleanup();
        error = host + " does not resolve";
        return false;
    }
    std::string addrs;
    {
        std::unique_lock lock(gAllowLock);
        gAllowNames.push_back(wide);
        addrs = addAddressesLocked(name, list, true);
    }
    freeaddrinfo(list);
    WSACleanup();
    log().info("allowHosts: join added {} ({})", host, addrs.empty() ? std::string("listed already or on the PC") : addrs);
    return true;
}

std::vector<std::string> allowedHosts() {
    std::shared_lock         lock(gAllowLock);
    std::vector<std::string> r;
    r.reserve(gAllowNames.size());
    for (auto const& n : gAllowNames) {
        std::string s;
        for (wchar_t c : n) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        r.push_back(std::move(s));
    }
    return r;
}

} // namespace bedd
