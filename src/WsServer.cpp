// The WebSocket server: an RFC 6455 subset on Winsock, the transport of every op (Pipe.cpp keeps the
// op dispatch), the tap (Tap.cpp) and the manager (Manager.cpp).
//
// * It listens on 127.0.0.1 only (config wsPort; 0 = an ephemeral port), at the path /v1.
// * A request must carry the token of this run (Discovery.cpp writes it next to the URL), as
//   "Authorization: Bearer <token>" or "?token=<token>", else HTTP 401. A request with an Origin
//   header (a web page) gets HTTP 403 unless config allowOrigins lists that origin.
// * Text, binary, ping/pong, close and fragmented messages; no extensions; a message larger than
//   16 MiB closes the connection with 1009.
// * Each connection has its own thread, which reads its messages and calls the handler for each in
//   order. Sockets are non-blocking and every wait polls, so stop() ends every thread within a
//   fraction of a second (bar a handler still busy with an op).
#include "WsServer.h"
#include "BeDD.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <list>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace bedd::ws {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr int    kPollMs         = 100;
constexpr size_t kMaxRequest     = 16 << 10;  // HTTP upgrade request, headers included
constexpr auto   kHandshakeLimit = 10s;       // to send the upgrade request
constexpr auto   kSendStall      = 30s;       // a peer that reads nothing for this long is dropped
constexpr int    kExtraThreads   = 8;         // connections still in their handshake, past maxClients

std::atomic<bool> gStopping{false};
Config            gConfig;
Handlers          gHandlers;
SOCKET            gListen = INVALID_SOCKET;
std::jthread      gAcceptor;
bool              gWsaStarted = false;

struct Slot {
    std::shared_ptr<Connection> conn; // set once upgraded
    std::jthread                thread;
    std::atomic<bool>           done{false};
};

std::mutex                       gSlotsMutex;
std::list<std::unique_ptr<Slot>> gSlots;   // guarded by gSlotsMutex
std::atomic<int>                 gUpgraded{0};
uint64_t                         gNextId = 0; // acceptor thread only

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Comma-separated header value contains `token` (case-insensitive), as Connection: keep-alive, Upgrade.
bool hasToken(std::string_view value, std::string_view token) {
    std::string v = lower(value), t = lower(token);
    size_t      start = 0;
    while (start <= v.size()) {
        size_t end = v.find(',', start);
        if (end == std::string::npos) end = v.size();
        if (trim(std::string_view(v).substr(start, end - start)) == t) return true;
        start = end + 1;
    }
    return false;
}

// Equal in a time that does not depend on where they differ (the token is the only secret).
bool constantTimeEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string base64(unsigned char const* data, size_t size) {
    static char const kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string       out;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < size) v |= data[i + 2];
        out += kChars[(v >> 18) & 63];
        out += kChars[(v >> 12) & 63];
        out += i + 1 < size ? kChars[(v >> 6) & 63] : '=';
        out += i + 2 < size ? kChars[v & 63] : '=';
    }
    return out;
}

// Sec-WebSocket-Accept: base64(SHA-1(key + the RFC 6455 GUID)), SHA-1 from CNG.
std::string acceptKey(std::string_view key) {
    std::string       input = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char     digest[20]{};
    BCRYPT_ALG_HANDLE alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0))
           && BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))
           && BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0))
           && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? base64(digest, sizeof(digest)) : std::string();
}

bool validUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        // n continuation bytes follow the lead byte.
        int n = c >= 0xF8 ? -1 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
        if (n < 0 || i + static_cast<size_t>(n) >= s.size()) return false;
        uint32_t cp = n == 3 ? c & 0x07u : n == 2 ? c & 0x0Fu : c & 0x1Fu;
        for (int k = 1; k <= n; ++k) {
            auto cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates and code points past U+10FFFF.
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF
            || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += static_cast<size_t>(n) + 1;
    }
    return true;
}

// Waits until the socket is readable/writable; false once the server stops (or `abort` is set).
enum class Wait { Ready, Timeout, Stop };
Wait waitFor(SOCKET s, short events, std::atomic<bool> const* abort) {
    WSAPOLLFD fd{s, events, 0};
    int       r = WSAPoll(&fd, 1, kPollMs);
    if (gStopping.load() || (abort && abort->load())) return Wait::Stop;
    if (r < 0) return Wait::Stop;
    return r == 0 ? Wait::Timeout : Wait::Ready;
}

// All of it, or false (the peer is gone, it stopped reading for kSendStall, or the server stops).
// `ignoreStop`: the close frame stop() sends to every connection.
bool sendAll(SOCKET s, std::string_view data, bool ignoreStop = false) {
    auto lastProgress = Clock::now();
    while (!data.empty()) {
        int n = ::send(s, data.data(), static_cast<int>(std::min<size_t>(data.size(), 1u << 20)), 0);
        if (n > 0) {
            data.remove_prefix(static_cast<size_t>(n));
            lastProgress = Clock::now();
            continue;
        }
        if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) return false;
        if (Clock::now() - lastProgress > (ignoreStop ? 1s : kSendStall)) return false;
        WSAPOLLFD fd{s, POLLWRNORM, 0};
        if (WSAPoll(&fd, 1, kPollMs) < 0) return false;
        if (gStopping.load() && !ignoreStop) return false;
    }
    return true;
}

// Buffered reads off a non-blocking socket.
struct Reader {
    SOCKET                   s;
    std::atomic<bool> const* abort = nullptr;
    std::string              buf;
    size_t                   pos = 0;

    // More bytes into buf; false when the peer closed, on an error, or the server stops.
    // `deadline` (if set): false past it as well.
    bool fill(Clock::time_point deadline = Clock::time_point::max()) {
        for (;;) {
            char chunk[16384];
            int  n = ::recv(s, chunk, sizeof(chunk), 0);
            if (n > 0) {
                if (pos > 0 && pos == buf.size()) {
                    buf.clear();
                    pos = 0;
                }
                buf.append(chunk, static_cast<size_t>(n));
                return true;
            }
            if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return false;
            if (waitFor(s, POLLRDNORM, abort) == Wait::Stop) return false;
            if (Clock::now() > deadline) return false;
        }
    }

    bool read(void* out, size_t n) {
        while (buf.size() - pos < n)
            if (!fill()) return false;
        std::memcpy(out, buf.data() + pos, n);
        pos += n;
        if (pos == buf.size()) {
            buf.clear();
            pos = 0;
        }
        return true;
    }

    bool read(std::string& out, size_t n) {
        out.resize(n);
        return n == 0 || read(out.data(), n);
    }
};

// ---------------------------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------------------------
void reject(SOCKET s, int status, char const* reason, std::string const& extra = {}) {
    std::string body = std::string(reason) + "\n";
    std::string resp = std::format(
        "HTTP/1.1 {} {}\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n{}\r\n{}",
        status,
        reason,
        body.size(),
        extra,
        body
    );
    sendAll(s, resp, true);
}

struct Request {
    std::string                                  method, target;
    std::unordered_map<std::string, std::string> headers; // lower-case names
};

bool parseRequest(std::string_view head, Request& req) {
    size_t eol = head.find("\r\n");
    if (eol == std::string_view::npos) return false;
    std::string_view line = head.substr(0, eol);
    size_t           sp1 = line.find(' '), sp2 = line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == sp1) return false;
    req.method = std::string(line.substr(0, sp1));
    req.target = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
    if (!line.substr(sp2 + 1).starts_with("HTTP/1.")) return false;
    size_t pos = eol + 2;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string_view::npos) end = head.size();
        std::string_view h     = head.substr(pos, end - pos);
        size_t           colon = h.find(':');
        if (colon != std::string_view::npos) {
            std::string name  = lower(trim(h.substr(0, colon)));
            std::string value = std::string(trim(h.substr(colon + 1)));
            auto [it, fresh]  = req.headers.try_emplace(name, value);
            if (!fresh) it->second += ", " + value;
        }
        pos = end + 2;
    }
    return true;
}

std::string header(Request const& req, char const* name) {
    auto it = req.headers.find(name);
    return it == req.headers.end() ? std::string() : it->second;
}

// The token from ?token=... (hex: nothing to percent-decode).
std::string queryToken(std::string_view query) {
    size_t start = 0;
    while (start < query.size()) {
        size_t           end = query.find('&', start);
        std::string_view kv  = query.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (kv.starts_with("token=")) return std::string(kv.substr(6));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

// Reads and checks the upgrade request; on success the 101 answer is sent. Bytes after the request
// stay in `in` (a client may send its first frame right away).
bool handshake(SOCKET s, Reader& in, std::string& who) {
    auto   deadline = Clock::now() + kHandshakeLimit;
    size_t end;
    while ((end = in.buf.find("\r\n\r\n")) == std::string::npos) {
        if (in.buf.size() > kMaxRequest) {
            reject(s, 431, "Request Header Fields Too Large");
            return false;
        }
        if (!in.fill(deadline)) return false;
    }
    Request req;
    if (!parseRequest(std::string_view(in.buf).substr(0, end + 2), req)) {
        reject(s, 400, "Bad Request");
        return false;
    }
    in.pos = end + 4;
    if (in.pos == in.buf.size()) {
        in.buf.clear();
        in.pos = 0;
    }

    std::string_view target(req.target);
    size_t           q     = target.find('?');
    std::string_view path  = target.substr(0, q);
    std::string_view query = q == std::string_view::npos ? std::string_view() : target.substr(q + 1);
    if (path != "/v1") {
        reject(s, 404, "Not Found");
        return false;
    }
    // A web page: only the origins config allowOrigins names (none by default).
    if (auto origin = req.headers.find("origin"); origin != req.headers.end()) {
        bool allowed = std::ranges::any_of(gConfig.allowOrigins, [&](std::string const& o) {
            return lower(o) == lower(origin->second);
        });
        if (!allowed) {
            log().warn("websocket: refused a request from origin '{}' (not in allowOrigins)", origin->second);
            reject(s, 403, "Forbidden");
            return false;
        }
    }
    std::string token = queryToken(query);
    if (std::string auth = header(req, "authorization"); !auth.empty()) {
        std::string_view a(auth);
        if (a.size() > 7 && lower(a.substr(0, 7)) == "bearer ") token = std::string(trim(a.substr(7)));
    }
    if (!constantTimeEqual(token, gConfig.token)) {
        reject(s, 401, "Unauthorized", "WWW-Authenticate: Bearer\r\n");
        return false;
    }
    if (req.method != "GET") {
        reject(s, 405, "Method Not Allowed", "Allow: GET\r\n");
        return false;
    }
    if (!hasToken(header(req, "upgrade"), "websocket") || !hasToken(header(req, "connection"), "upgrade")) {
        reject(s, 426, "Upgrade Required", "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n");
        return false;
    }
    if (header(req, "sec-websocket-version") != "13") {
        reject(s, 426, "Upgrade Required", "Sec-WebSocket-Version: 13\r\n");
        return false;
    }
    std::string key = header(req, "sec-websocket-key");
    if (key.empty()) {
        reject(s, 400, "Bad Request");
        return false;
    }
    if (gUpgraded.fetch_add(1) >= gConfig.maxClients) {
        --gUpgraded;
        log().warn("websocket: refused a client: {} connected already (wsMaxClients)", gConfig.maxClients);
        reject(s, 503, "Service Unavailable");
        return false;
    }
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                     + acceptKey(key) + "\r\n\r\n";
    if (!sendAll(s, resp)) {
        --gUpgraded;
        return false;
    }
    who = header(req, "user-agent");
    return true;
}

// ---------------------------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------------------------
enum : uint8_t { kContinuation = 0x0, kClose = 0x8, kPing = 0x9, kPong = 0xA };

// Reads messages until the connection ends; returns the close code to answer with (0: none).
uint16_t readMessages(std::shared_ptr<Connection> const& conn, Reader& in) {
    std::string message;
    uint8_t     messageOpcode = 0; // 0: none in progress
    std::string payload;
    for (;;) {
        if (conn->closed()) return 0;
        uint8_t h[2];
        if (!in.read(h, 2)) return 0;
        bool    fin    = h[0] & 0x80;
        uint8_t opcode = h[0] & 0x0F;
        if ((h[0] & 0x70) || !(h[1] & 0x80)) return 1002; // no extensions; client frames are masked
        uint64_t len = h[1] & 0x7F;
        if (len == 126) {
            uint8_t b[2];
            if (!in.read(b, 2)) return 0;
            len = (static_cast<uint64_t>(b[0]) << 8) | b[1];
        } else if (len == 127) {
            uint8_t b[8];
            if (!in.read(b, 8)) return 0;
            len = 0;
            for (uint8_t x : b) len = (len << 8) | x;
        }
        bool control = opcode & 0x8;
        if (control && (!fin || len > 125)) return 1002;
        if (!control && (len > kMaxMessage || message.size() + len > kMaxMessage)) {
            log().warn("websocket client {}: a message over {} MiB; closing it", conn->id(), kMaxMessage >> 20);
            return 1009;
        }
        uint8_t mask[4];
        if (!in.read(mask, 4) || !in.read(payload, static_cast<size_t>(len))) return 0;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);

        switch (opcode) {
        case kClose: {
            uint16_t code = 1000;
            if (payload.size() >= 2) code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]));
            else if (payload.size() == 1) return 1002;
            // Echo the code (the reserved ones the peer must not send become 1002).
            bool valid = (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) || (code >= 3000 && code <= 4999);
            return valid ? code : 1002;
        }
        case kPing: conn->send(kPong, {payload}); continue;
        case kPong: continue;
        case kContinuation:
            if (!messageOpcode) return 1002;
            message += payload;
            break;
        case kText:
        case kBinary:
            if (messageOpcode) return 1002;
            messageOpcode = opcode;
            message       = std::move(payload);
            payload.clear();
            break;
        default: return 1002;
        }
        if (!fin) continue;
        if (messageOpcode == kText && !validUtf8(message)) return 1007;
        uint8_t op    = messageOpcode;
        messageOpcode = 0;
        if (gHandlers.message) gHandlers.message(conn, op, std::move(message));
        message.clear();
    }
}

void serveSocket(Slot& slot, SOCKET s, uint64_t id) {
    Reader      in{s};
    std::string who;
    if (!handshake(s, in, who)) {
        shutdown(s, SD_SEND);
        closesocket(s);
        return;
    }
    auto conn = std::make_shared<Connection>(static_cast<uintptr_t>(s), id);
    {
        std::lock_guard lock(gSlotsMutex);
        slot.conn = conn;
    }
    in.abort = conn->closedFlag(); // a read in progress ends once the connection is closed (tap DROPPED)
    if (options().trace) log().info("websocket client {} connected ({})", id, who);
    uint16_t code = 0;
    try {
        if (gHandlers.open) gHandlers.open(conn);
        code = readMessages(conn, in);
    } catch (std::exception const& e) {
        // Still the whole disconnect path below: the client's held input must be released.
        log().error("websocket client {}: {}", id, e.what());
        code = 1011;
    }
    if (code) conn->close(code, code == 1009 ? "message too big" : code == 1002 ? "protocol error" : code == 1007 ? "invalid UTF-8" : "");
    else conn->close(1000, "");
    conn->markClosed();
    --gUpgraded;
    // Let the peer read what was sent (a DROPPED marker, the close frame): half-close, then wait a
    // moment for it to hang up rather than resetting the connection with unread data.
    shutdown(s, SD_SEND);
    auto until = Clock::now() + (gStopping ? 200ms : 1s);
    for (char sink[4096]; Clock::now() < until;) {
        int n = ::recv(s, sink, sizeof(sink), 0);
        if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) break;
        if (n < 0) {
            WSAPOLLFD fd{s, POLLRDNORM, 0};
            WSAPoll(&fd, 1, 50);
        }
    }
    if (options().trace) log().info("websocket client {} disconnected", id);
    try {
        if (gHandlers.closed) gHandlers.closed(conn, gStopping.load());
    } catch (std::exception const& e) {
        log().error("websocket client {}: {}", id, e.what());
    }
    // The socket is closed with the last reference (the tap writer may still hold one).
}

void runSlot(Slot& slot, SOCKET s, uint64_t id) {
    try {
        serveSocket(slot, s, id);
    } catch (std::exception const& e) {
        log().error("websocket client {}: {}", id, e.what());
    }
    std::lock_guard lock(gSlotsMutex);
    slot.conn.reset();
    slot.done = true;
}

// Joins the threads of connections that ended (gSlotsMutex held).
void reap() {
    for (auto it = gSlots.begin(); it != gSlots.end();) {
        if ((*it)->done) {
            (*it)->thread.join();
            it = gSlots.erase(it);
        } else {
            ++it;
        }
    }
}

void acceptLoop(std::stop_token) {
    while (!gStopping) {
        WSAPOLLFD fd{gListen, POLLRDNORM, 0};
        int       r = WSAPoll(&fd, 1, 200);
        {
            std::lock_guard lock(gSlotsMutex);
            reap();
        }
        if (gStopping) break;
        if (r <= 0) continue;
        SOCKET s = accept(gListen, nullptr, nullptr);
        if (s == INVALID_SOCKET) continue;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&noDelay), sizeof(noDelay));
        bool full = false;
        {
            std::lock_guard lock(gSlotsMutex);
            full = static_cast<int>(gSlots.size()) >= gConfig.maxClients + kExtraThreads;
            if (!full) {
                auto& slot   = gSlots.emplace_back(std::make_unique<Slot>());
                slot->thread = std::jthread([p = slot.get(), s, id = ++gNextId] { runSlot(*p, s, id); });
            }
        }
        if (full) {
            reject(s, 503, "Service Unavailable");
            closesocket(s);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------------------------
Connection::Connection(uintptr_t socket, uint64_t id) : mSocket(socket), mId(id) {}

Connection::~Connection() { closesocket(static_cast<SOCKET>(mSocket)); }

bool Connection::sendFrame(uint8_t opcode, std::initializer_list<std::string_view> parts) {
    uint64_t len = 0;
    for (auto p : parts) len += p.size();
    std::string head;
    head += static_cast<char>(0x80 | opcode);
    if (len < 126) {
        head += static_cast<char>(len);
    } else if (len <= 0xFFFF) {
        head += static_cast<char>(126);
        head += static_cast<char>(len >> 8);
        head += static_cast<char>(len & 0xFF);
    } else {
        head += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) head += static_cast<char>((len >> (i * 8)) & 0xFF);
    }
    bool       closing = opcode == kClose;
    auto const s       = static_cast<SOCKET>(mSocket);
    if (!sendAll(s, head, closing)) return false;
    for (auto p : parts)
        if (!sendAll(s, p, closing)) return false;
    return true;
}

bool Connection::send(uint8_t opcode, std::initializer_list<std::string_view> parts) {
    std::lock_guard lock(mSendMutex);
    if (mClosed.load() || mCloseSent) return false;
    if (sendFrame(opcode, parts)) return true;
    mClosed = true; // a frame cut short: nothing after it would make sense to the peer
    return false;
}

void Connection::close(uint16_t code, std::string_view reason) {
    std::lock_guard lock(mSendMutex);
    mClosed = true;
    if (mCloseSent) return;
    mCloseSent = true;
    std::string body;
    body += static_cast<char>(code >> 8);
    body += static_cast<char>(code & 0xFF);
    body += reason.substr(0, 123);
    sendFrame(kClose, {body});
}

// ---------------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------------
int start(Config config, Handlers handlers) {
    if (gAcceptor.joinable()) return 0;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log().error("websocket: WSAStartup failed");
        return 0;
    }
    gWsaStarted = true;
    gConfig     = std::move(config);
    gConfig.maxClients = std::max(1, gConfig.maxClients);
    gHandlers   = std::move(handlers);
    gStopping   = false;

    gListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (gListen == INVALID_SOCKET) {
        log().error("websocket: socket() failed ({})", WSAGetLastError());
        return 0;
    }
    // Another process must not take over the port while we listen on it.
    BOOL exclusive = TRUE;
    setsockopt(gListen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&exclusive), sizeof(exclusive));
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(static_cast<u_short>(std::clamp(gConfig.port, 0, 65535)));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(gListen, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(gListen, SOMAXCONN) != 0) {
        log().error("websocket: could not listen on 127.0.0.1:{} ({})", gConfig.port, WSAGetLastError());
        closesocket(gListen);
        gListen = INVALID_SOCKET;
        return 0;
    }
    u_long nonBlocking = 1;
    ioctlsocket(gListen, FIONBIO, &nonBlocking);
    int len = sizeof(a);
    getsockname(gListen, reinterpret_cast<sockaddr*>(&a), &len);
    gAcceptor = std::jthread(acceptLoop);
    return ntohs(a.sin_port);
}

void stop() {
    if (gAcceptor.joinable()) {
        gStopping = true;
        gAcceptor.join();
    }
    if (gListen != INVALID_SOCKET) {
        closesocket(gListen);
        gListen = INVALID_SOCKET;
    }
    std::list<std::unique_ptr<Slot>>        slots;
    std::vector<std::shared_ptr<Connection>> open;
    {
        std::lock_guard lock(gSlotsMutex);
        slots.swap(gSlots);
        for (auto& slot : slots)
            if (slot->conn) open.push_back(slot->conn);
    }
    // Outside the lock: a close waits for a message being sent (which gives up now that we stop).
    for (auto& conn : open) conn->close(1001, "BeDD is unloading");
    open.clear();
    for (auto& slot : slots) slot->thread.join();
    gHandlers = {};
    if (gWsaStarted) {
        WSACleanup();
        gWsaStarted = false;
    }
}

int connections() { return gUpgraded.load(); }

std::string newToken() {
    unsigned char bytes[32]{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("BCryptGenRandom failed");
    static char const kHex[] = "0123456789abcdef";
    std::string       out;
    for (unsigned char b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

} // namespace bedd::ws
