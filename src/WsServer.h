#pragma once
// The WebSocket server (WsServer.cpp): the mod's only transport. A small RFC 6455 subset on
// Winsock, bound to 127.0.0.1: text, binary, ping/pong, close and fragmented messages; no
// extensions. Each connection is served by its own thread, which calls the handler for each
// message in order (so a client's requests are answered in the order it sent them).
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bedd::ws {

constexpr uint8_t kText   = 0x1;
constexpr uint8_t kBinary = 0x2;

/// Largest message a client may send (and the frame size the server accepts): bigger ones close
/// the connection with 1009.
constexpr size_t kMaxMessage = 16u << 20;

/// One accepted WebSocket connection. Senders may be any thread (a reply on the connection's own
/// thread, tap frames on the tap writer): whole messages never interleave.
class Connection {
public:
    explicit Connection(uintptr_t socket, uint64_t id);
    ~Connection();
    Connection(Connection const&)            = delete;
    Connection& operator=(Connection const&) = delete;

    /// Ids start at 1 and are never reused.
    [[nodiscard]] uint64_t id() const { return mId; }
    /// One message made of the parts (no copy into one buffer). False once the connection is closed
    /// or the peer stopped reading for good.
    bool send(uint8_t opcode, std::initializer_list<std::string_view> parts);
    bool sendText(std::string_view text) { return send(kText, {text}); }
    /// Sends a close frame (once) and ends the connection: its thread leaves at its next read.
    void close(uint16_t code, std::string_view reason);
    [[nodiscard]] bool closed() const { return mClosed.load(); }

    // WsServer.cpp only.
    bool sendFrame(uint8_t opcode, std::initializer_list<std::string_view> parts);
    void markClosed() { mClosed = true; }
    [[nodiscard]] uintptr_t socket() const { return mSocket; }
    [[nodiscard]] std::atomic<bool> const* closedFlag() const { return &mClosed; }

private:
    uintptr_t         mSocket;
    uint64_t          mId;
    std::mutex        mSendMutex;
    std::atomic<bool> mClosed{false};
    bool              mCloseSent = false; // guarded by mSendMutex
};

struct Handlers {
    /// On the connection's thread, once it is upgraded (before its first message).
    std::function<void(std::shared_ptr<Connection> const&)> open;
    /// On the connection's thread, for each complete message (opcode kText or kBinary).
    std::function<void(std::shared_ptr<Connection> const&, uint8_t opcode, std::string&& data)> message;
    /// On the connection's thread once it ended; `stopping` while the server shuts down.
    std::function<void(std::shared_ptr<Connection> const&, bool stopping)> closed;
};

struct Config {
    int                      port       = 0; // 0 = an ephemeral port
    int                      maxClients = 8; // upgraded connections at once (more: HTTP 503)
    std::string              token;          // 64 hex characters
    std::vector<std::string> allowOrigins;   // requests with another Origin header: HTTP 403
};

/// Binds 127.0.0.1 and starts accepting. Returns the bound port, or 0 when it could not listen.
int  start(Config config, Handlers handlers);
/// Closes the listener and every connection (close 1001) and joins their threads. Handlers that
/// are still running must return on their own (disable() wakes the waiting ops first).
void stop();
/// Upgraded connections now.
[[nodiscard]] int connections();

/// 32 random bytes (BCryptGenRandom) as 64 lower-case hex characters.
[[nodiscard]] std::string newToken();

} // namespace bedd::ws
